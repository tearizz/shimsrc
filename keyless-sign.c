// SPDX-License-Identifier: BSD-2-Clause-Patent

/*
 * Simplified Authenticode data extraction:
 * - Read raw on-disk bytes
 * - Parse PE header to locate Security Directory  
 * - Compute Authenticode hash over file bytes excluding Security Directory
 * - Parse PKCS#7 from Security Directory
 * - Extract SpcIndirectDataContent (SEQ content) and verify messageDigest
 * - Extract SignedAttributes DER, signature, and certificate for external verification
 */

#include "keyless-sign.h"
#include "http-request.h"
#include "include/pe.h"
#include "keyless-sup.h"
#include "shim.h"

#include <limits.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>

/* Defensive cap for one HTTP POST body (base64 signature material + JSON). */
#ifndef OSIGN_MAX_REQUEST_SIZE
#define OSIGN_MAX_REQUEST_SIZE (8U * 1024U * 1024U)
#endif
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

static EFI_STATUS
read_file_ondisk(EFI_HANDLE device, CHAR16 *path, void **buf, UINTN *sz)
{
	EFI_STATUS efi_status;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
	EFI_FILE_PROTOCOL *root = NULL, *fh = NULL;
	EFI_FILE_INFO *info = NULL;
	UINTN infosz = sizeof(EFI_FILE_INFO);

	*buf = NULL;
	*sz = 0;

	efi_status = BS->HandleProtocol(device, &EFI_SIMPLE_FILE_SYSTEM_GUID,
	                                (void **)&fs);
	if (EFI_ERROR(efi_status))
		return efi_status;

	efi_status = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(efi_status))
		return efi_status;

	efi_status = root->Open(root, &fh, path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(efi_status))
		goto done;

	info = AllocatePool(infosz);
	if (!info) {
		efi_status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	efi_status = fh->GetInfo(fh, &EFI_FILE_INFO_GUID, &infosz, info);
	if (efi_status == EFI_BUFFER_TOO_SMALL) {
		FreePool(info);
		info = AllocatePool(infosz);
		if (!info) {
			efi_status = EFI_OUT_OF_RESOURCES;
			goto done;
		}
		efi_status =
			fh->GetInfo(fh, &EFI_FILE_INFO_GUID, &infosz, info);
	}
	if (EFI_ERROR(efi_status))
		goto done;

	if (info->FileSize > INT_MAX) {
		efi_status = EFI_BAD_BUFFER_SIZE;
		goto done;
	}
	*sz = info->FileSize;
	*buf = AllocatePool(*sz);
	if (!*buf) {
		efi_status = EFI_OUT_OF_RESOURCES;
		goto done;
	}

	efi_status = fh->Read(fh, sz, *buf);

done:
	if (fh)
		fh->Close(fh);
	if (root)
		root->Close(root);
	if (info)
		FreePool(info);
	if (EFI_ERROR(efi_status) && *buf) {
		FreePool(*buf);
		*buf = NULL;
		*sz = 0;
	}
	return efi_status;
}

/*
 * A Fulcio/keyless leaf certificate carries the OIDC issuer extension
 * 1.3.6.1.4.1.57264.1.1.  This check is intentionally cheap and is done
 * before parsing the SpcIndirectDataContent / allocating base64 buffers, so
 * keyed-only artifacts do not pay the keyless-extraction cost.
 */
static BOOLEAN
cert_is_keyless(X509 *signer)
{
	ASN1_OBJECT *fulcio_oid;
	BOOLEAN keyless = FALSE;

	if (!signer)
		return FALSE;

	fulcio_oid = OBJ_txt2obj("1.3.6.1.4.1.57264.1.1", 1);
	if (!fulcio_oid)
		return FALSE;

	keyless = (X509_get_ext_by_OBJ(signer, fulcio_oid, -1) >= 0);
	ASN1_OBJECT_free(fulcio_oid);
	return keyless;
}

static void
osign_free_signature_blobs(OSIGN_SIGNATURE *sigs, UINTN count)
{
	if (!sigs)
		return;

	for (UINTN i = 0; i < count; i++) {
		if (sigs[i].payload)
			FreePool(sigs[i].payload);
		if (sigs[i].signature)
			FreePool(sigs[i].signature);
		if (sigs[i].certificate)
			FreePool(sigs[i].certificate);
	}
}

/* Check the image digest independently of the SignerInfo digest algorithm. */
static BOOLEAN
match_image_digest(const unsigned char *content, long size,
                   const UINT8 *sha256, const UINT8 *sha1)
{
	const unsigned char *p = content;
	const unsigned char *end = content + size;
	const ASN1_OCTET_STRING *digest;
	const X509_ALGOR *algorithm;
	const ASN1_OBJECT *oid;
	const UINT8 *expected;
	int expected_size;
	int tag, xclass;
	long len;
	int flags;
	BOOLEAN match = FALSE;

	/* SpcAttributeTypeAndOptionalValue precedes DigestInfo. */
	flags = ASN1_get_object(&p, &len, &tag, &xclass, size);
	if (flags != V_ASN1_CONSTRUCTED || xclass != V_ASN1_UNIVERSAL ||
	    tag != V_ASN1_SEQUENCE || len < 0 || len >= end - p)
		return FALSE;
	p += len;

	X509_SIG *info = d2i_X509_SIG(NULL, &p, end - p);
	if (!info)
		return FALSE;
	if (p != end)
		goto out;
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	X509_SIG_get0(info, &algorithm, &digest);
	if (!algorithm)
		goto out;
	X509_ALGOR_get0(&oid, NULL, NULL, algorithm);
#else
	algorithm = info->algor;
	digest = info->digest;
	oid = algorithm ? algorithm->algorithm : NULL;
#endif
	if (!oid || !digest || !digest->data)
		goto out;

	switch (OBJ_obj2nid(oid)) {
	case NID_sha256:
		expected = sha256;
		expected_size = SHA256_DIGEST_SIZE;
		break;
	case NID_sha1:
		expected = sha1;
		expected_size = SHA1_DIGEST_SIZE;
		break;
	default:
		goto out;
	}
	match = digest->length == expected_size &&
	        memcmp(digest->data, expected, expected_size) == 0;
out:
	X509_SIG_free(info);
	return match;
}

static EFI_STATUS
extract_verification_data(PKCS7 *p7, const UINT8 *sha256, const UINT8 *sha1,
                          OSIGN_SIGNATURE *sig)
{
	static const char spc_oid[] = "1.3.6.1.4.1.311.2.1.4";
	char oid_text[sizeof(spc_oid)];
	PKCS7_SIGNED *sd;
	PKCS7_SIGNER_INFO *si;
	PKCS7 *contents;
	X509 *signer;
	ASN1_STRING *sequence;
	const ASN1_TYPE *attr;
	const ASN1_OCTET_STRING *message_digest;
	const unsigned char *p;
	UINT8 hash[SHA256_DIGEST_SIZE];
	int hash_size, tag, xclass, flags;
	long len;
	unsigned char *der = NULL;
	int der_size;

	if (!p7 || !PKCS7_type_is_signed(p7))
		return EFI_NOT_FOUND;
	sd = p7->d.sign;
	if (!sd || !sd->signer_info ||
	    sk_PKCS7_SIGNER_INFO_num(sd->signer_info) <= 0)
		return EFI_NOT_FOUND;
	si = sk_PKCS7_SIGNER_INFO_value(sd->signer_info, 0);
	if (!si)
		return EFI_NOT_FOUND;
	signer = PKCS7_cert_from_signer_info(p7, si);
	if (!cert_is_keyless(signer))
		return EFI_NOT_FOUND;

	contents = sd->contents;
	if (!contents || !contents->type ||
	    OBJ_obj2txt(oid_text, sizeof(oid_text), contents->type, 1) != sizeof(spc_oid) - 1 ||
	    memcmp(oid_text, spc_oid, sizeof(spc_oid)) ||
	    !contents->d.other || contents->d.other->type != V_ASN1_SEQUENCE)
		return EFI_NOT_FOUND;
	sequence = contents->d.other->value.sequence;
	if (!sequence || !sequence->data || sequence->length <= 0)
		return EFI_NOT_FOUND;

	p = sequence->data;
	flags = ASN1_get_object(&p, &len, &tag, &xclass, sequence->length);
	if (flags != V_ASN1_CONSTRUCTED || xclass != V_ASN1_UNIVERSAL ||
	    tag != V_ASN1_SEQUENCE || len <= 0 ||
	    len != sequence->data + sequence->length - p)
		return EFI_NOT_FOUND;

	if (!si->digest_alg || !si->digest_alg->algorithm)
		return EFI_NOT_FOUND;
	switch (OBJ_obj2nid(si->digest_alg->algorithm)) {
	case NID_sha256:
		if (!SHA256(p, len, hash))
			return EFI_DEVICE_ERROR;
		hash_size = SHA256_DIGEST_SIZE;
		break;
	case NID_sha1:
		if (!SHA1(p, len, hash))
			return EFI_DEVICE_ERROR;
		hash_size = SHA1_DIGEST_SIZE;
		break;
	default:
		return EFI_NOT_FOUND;
	}

	attr = PKCS7_get_signed_attribute(si, NID_pkcs9_messageDigest);
	if (!attr || attr->type != V_ASN1_OCTET_STRING)
		return EFI_NOT_FOUND;
	message_digest = attr->value.octet_string;
	if (!message_digest || !message_digest->data ||
	    message_digest->length != hash_size ||
	    memcmp(message_digest->data, hash, hash_size) ||
	    !match_image_digest(p, len, sha256, sha1) ||
	    !si->enc_digest || !si->enc_digest->data || si->enc_digest->length <= 0)
		return EFI_NOT_FOUND;

	/* Use the same attribute encoding and order as OpenSSL verification. */
	der_size = ASN1_item_i2d((ASN1_VALUE *)si->auth_attr, &der,
	                        ASN1_ITEM_rptr(PKCS7_ATTR_VERIFY));
	if (der_size > 0 && der)
		sig->payload = b64_encode(der, der_size);
	OPENSSL_free(der);
	if (!sig->payload)
		return EFI_OUT_OF_RESOURCES;

	sig->signature = b64_encode(si->enc_digest->data, si->enc_digest->length);
	if (!sig->signature)
		return EFI_OUT_OF_RESOURCES;

	der = NULL;
	der_size = i2d_X509(signer, &der);
	if (der_size > 0 && der)
		sig->certificate = b64_encode(der, der_size);
	OPENSSL_free(der);
	return sig->certificate ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static EFI_STATUS
osign_extract_data(EFI_HANDLE image_handle, CHAR16 *path, CHAR8 **data,
                   UINTN *datasize, PE_COFF_LOADER_IMAGE_CONTEXT *ctx)
{
	EFI_STATUS efi_status;
	EFI_LOADED_IMAGE *li = NULL;

	efi_status = BS->HandleProtocol(image_handle, &EFI_LOADED_IMAGE_GUID,
	                                (void **)&li);
	if (EFI_ERROR(efi_status))
		return efi_status;
	if (!li)
		return EFI_NOT_FOUND;

	/* read_file_ondisk expects a void** for the buffer pointer */
	*data = NULL;
	*datasize = 0;
	efi_status = read_file_ondisk(li->DeviceHandle, path, (void **)data,
	                              datasize);
	if (EFI_ERROR(efi_status)) {
		return efi_status;
	}

	/* pass the actual buffer and size (data and datasize are pointers) */
	efi_status = read_header(*data, (unsigned)(*datasize), ctx);
	if (EFI_ERROR(efi_status)) {
		goto out;
	}
	if (!ctx->SecDir || ctx->SecDir->Size == 0) {
		efi_status = EFI_NOT_FOUND;
		goto out;
	}

out:
	/* Caller is responsible for freeing *data */
	return efi_status;
}

EFI_STATUS
osign_parse_pkcs7(char *data, UINTN datasize,
                  const PE_COFF_LOADER_IMAGE_CONTEXT *ctx,
                  const UINT8 *sha256, const UINT8 *sha1,
                  OSIGN_SIGNATURE **out_sigs, UINTN *out_count)
{
	EFI_STATUS status;
	OSIGN_SIGNATURE *sigs = NULL;
	OSIGN_SIGNATURE sig = { 0 };
	UINTN count = 0, capacity = 0;
	UINTN start, size, offset = 0;

	if (!out_sigs || !out_count)
		return EFI_INVALID_PARAMETER;
	*out_sigs = NULL;
	*out_count = 0;
	if (!data || !ctx || !ctx->SecDir || !sha256 || !sha1)
		return EFI_INVALID_PARAMETER;

	start = ctx->SecDir->VirtualAddress;
	size = ctx->SecDir->Size;
	if (!size)
		return EFI_NOT_FOUND;
	if (start > datasize || size != datasize - start || start % 8)
		return EFI_INVALID_PARAMETER;

	while (offset < size) {
		WIN_CERTIFICATE header;
		UINTN remaining = size - offset;
		UINTN padding;
		const unsigned char *record;

		if (remaining < sizeof(header)) {
			status = EFI_INVALID_PARAMETER;
			goto fail;
		}
		record = (const unsigned char *)data + start + offset;
		memcpy(&header, record, sizeof(header));
		if (header.dwLength < sizeof(header) || header.dwLength > remaining) {
			status = EFI_INVALID_PARAMETER;
			goto fail;
		}

		padding = (8 - header.dwLength % 8) % 8;
		if (padding > remaining - header.dwLength) {
			status = EFI_INVALID_PARAMETER;
			goto fail;
		}
		offset += header.dwLength + padding;
		if (header.wCertificateType != WIN_CERT_TYPE_PKCS_SIGNED_DATA ||
		    header.dwLength == sizeof(header))
			continue;

		const unsigned char *der = record + sizeof(header);
		UINTN der_size = header.dwLength - sizeof(header);
		if (der_size > LONG_MAX) {
			status = EFI_INVALID_PARAMETER;
			goto fail;
		}
		PKCS7 *p7 = d2i_PKCS7(NULL, &der, (long)der_size);
		if (!p7)
			continue;
		status = extract_verification_data(p7, sha256, sha1, &sig);
		PKCS7_free(p7);
		if (status == EFI_NOT_FOUND)
			continue;
		if (EFI_ERROR(status))
			goto fail;

		if (count == OSIGN_MAX_SIGNATURE_COUNT) {
			status = EFI_BUFFER_TOO_SMALL;
			goto fail;
		}
		if (count == capacity) {
			UINTN next = capacity ? capacity * 2 : 4;
			if (next > OSIGN_MAX_SIGNATURE_COUNT)
				next = OSIGN_MAX_SIGNATURE_COUNT;
			OSIGN_SIGNATURE *larger = AllocatePool(next * sizeof(*sigs));
			if (!larger) {
				status = EFI_OUT_OF_RESOURCES;
				goto fail;
			}
			if (sigs) {
				CopyMem(larger, sigs, count * sizeof(*sigs));
				FreePool(sigs);
			}
			sigs = larger;
			capacity = next;
		}
		sigs[count++] = sig;
		sig = (OSIGN_SIGNATURE){ 0 };
	}

	if (count == 0)
		return EFI_NOT_FOUND;
	*out_sigs = sigs;
	*out_count = count;
	return EFI_SUCCESS;

fail:
	osign_free_signature_blobs(&sig, 1);
	osign_free_signature_blobs(sigs, count);
	if (sigs)
		FreePool(sigs);
	return status;
}

/* Decode cert (base64 DER) and return public key PEM as allocated CHAR8*.
 * Caller must FreePool() the returned pointer. Returns NULL on error.
 */
CHAR8 *
cert_b64_to_pubkey_pem(const CHAR8 *cert_b64)
{
	if (!cert_b64)
		return NULL;

	/* estimate DER max size and allocate temporary buffer */
	size_t b64len = AsciiStrLen((const char *)cert_b64);
	int der_max = (int)((b64len * 3) / 4 + 16);
	unsigned char *der = AllocatePool(der_max);
	if (!der)
		return NULL;

	/* Base64 decode using OpenSSL BIO */
	BIO *b64 = BIO_new(BIO_f_base64());
	BIO *bmem = BIO_new_mem_buf((void *)cert_b64,
	                            (int)AsciiStrLen((const char *)cert_b64));
	if (!b64 || !bmem) {
		if (b64)
			BIO_free(b64);
		if (bmem)
			BIO_free(bmem);
		FreePool(der);
		return NULL;
	}
	/* handle no-newline base64 input */
#ifdef BIO_FLAGS_BASE64_NO_NL
	BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
#endif
	bmem = BIO_push(b64, bmem);
	int der_len = BIO_read(bmem, der, der_max);
	BIO_free_all(bmem);
	if (der_len <= 0) {
		FreePool(der);
		return NULL;
	}

	/* Parse X509 from DER bytes */
	const unsigned char *p = der;
	X509 *x = d2i_X509(NULL, &p, der_len);
	FreePool(der);
	if (!x)
		return NULL;

	/* Extract public key (EVP_PKEY) */
	EVP_PKEY *pkey = X509_get_pubkey(x);
	X509_free(x);
	if (!pkey)
		return NULL;

	/* Write public key to PEM in memory BIO */
	BIO *out = BIO_new(BIO_s_mem());
	if (!out) {
		EVP_PKEY_free(pkey);
		return NULL;
	}
	if (!PEM_write_bio_PUBKEY(out, pkey)) {
		BIO_free(out);
		EVP_PKEY_free(pkey);
		return NULL;
	}
	EVP_PKEY_free(pkey);

	/* Extract PEM data from BIO and copy to AllocatePool buffer */
	char *pem_ptr = NULL;
	long pem_len = BIO_get_mem_data(out, &pem_ptr);
	if (pem_len <= 0 || !pem_ptr) {
		BIO_free(out);
		return NULL;
	}
	CHAR8 *ret = AllocatePool((UINTN)pem_len + 1);
	if (ret) {
		CopyMem(ret, pem_ptr, (UINTN)pem_len);
		ret[pem_len] = 0;
	}
	BIO_free(out);
	return ret;
}

BOOLEAN
osign_verify(EFI_HANDLE image_handle, CHAR16 *target_path)
{
	EFI_STATUS status;
	UINTN datasize = 0;
	CHAR8 *data = NULL;
	PE_COFF_LOADER_IMAGE_CONTEXT ctx;
	UINT8 sha256[SHA256_DIGEST_SIZE], sha1[SHA1_DIGEST_SIZE];
	OSIGN_SIGNATURE *sigs = NULL;
	UINTN sig_count = 0;

	status = osign_extract_data(image_handle, target_path, &data, &datasize,
	                            &ctx);
	if (EFI_ERROR(status))
		goto out;
	status = generate_hash((char *)data, (unsigned int)datasize, &ctx,
	                       sha256, sha1);
	if (EFI_ERROR(status))
		goto out;
	status = osign_parse_pkcs7((char *)data, datasize, &ctx, sha256, sha1,
	                           &sigs, &sig_count);
	if (EFI_ERROR(status))
		goto out;

	status = osign_http_request(image_handle, sigs, sig_count);
	/* The request frees the blobs, and this caller owns the array. */
	FreePool(sigs);
out:
	if (data)
		FreePool(data);
	return !EFI_ERROR(status);
}

EFI_STATUS
osign_http_request(EFI_HANDLE image_handle, OSIGN_SIGNATURE *sigs, UINTN count)
{
	CHAR8 *uri = NULL;
	CHAR8 *uri_literal =
		(CHAR8 *)"http://10.0.2.2:28082/v1/verify/authenticode";
	UINTN uri_len = AsciiStrLen((const char *)uri_literal);
	EFI_STATUS efi_status = EFI_SUCCESS;

	if (!sigs || count == 0)
		return EFI_INVALID_PARAMETER;

	uri = AllocatePool(uri_len + 1);
	if (!uri) {
		osign_free_signature_blobs(sigs, count);
		return EFI_OUT_OF_RESOURCES;
	}
	CopyMem(uri, uri_literal, uri_len);
	uri[uri_len] = '\0';

	/* GLOBAL VARIABLE: http_request_method */
	http_request_method = HttpMethodPost;

	/* HTTP body: {"signatures":[{"payload":..,"signature":..,"certificate":..},..]} */
	UINTN json_len = 50000;
	for (UINTN i = 0; i < count; i++) {
		UINTN add = 0;

		if (sigs[i].payload)
			add += AsciiStrLen((const char *)sigs[i].payload);
		if (sigs[i].signature)
			add += AsciiStrLen((const char *)sigs[i].signature);
		if (sigs[i].certificate)
			add += AsciiStrLen((const char *)sigs[i].certificate);

		if (add > (UINTN)-1 - json_len) {
			efi_status = EFI_OUT_OF_RESOURCES;
			goto free_blobs;
		}
		json_len += add;
		if (json_len > OSIGN_MAX_REQUEST_SIZE) {
			efi_status = EFI_BUFFER_TOO_SMALL;
			goto free_blobs;
		}
	}

	/* GLOBAL VARIABLE: tx_body_json */
	tx_body_json = AllocatePool(json_len + 1);
	if (!tx_body_json) {
		efi_status = EFI_OUT_OF_RESOURCES;
		goto free_blobs;
	}

	UINTN off = 0;
	AsciiSPrint(tx_body_json, json_len + 1, (CHAR8 *)"{\"signatures\":[");
	off = AsciiStrLen((const char *)tx_body_json);
	for (UINTN i = 0; i < count; i++) {
		if (off > json_len) {
			efi_status = EFI_BUFFER_TOO_SMALL;
			goto free_body;
		}
		AsciiSPrint(
			tx_body_json + off, json_len + 1 - off,
			(CHAR8 *)"%s{\"payload\":\"%a\",\"signature\":\"%a\",\"certificate\":\"%a\"}",
			i ? (CHAR8 *)"," : (CHAR8 *)"",
			sigs[i].payload ? sigs[i].payload : (CHAR8 *)"",
			sigs[i].signature ? sigs[i].signature : (CHAR8 *)"",
			sigs[i].certificate ? sigs[i].certificate
					    : (CHAR8 *)"");
		off = AsciiStrLen((const char *)tx_body_json);
	}
	if (off > json_len) {
		efi_status = EFI_BUFFER_TOO_SMALL;
		goto free_body;
	}
	AsciiSPrint(tx_body_json + off, json_len + 1 - off, (CHAR8 *)"]}");

	efi_status = send_http_get_request(image_handle, uri);
	if (EFI_ERROR(efi_status))
		perror(L"failed to send http request\n");

free_body:
	/* tx_body_json ownership: free now */
	if (tx_body_json) {
		FreePool(tx_body_json);
		tx_body_json = NULL;
	}

free_blobs:
	/* Free the collected signature blobs; the caller owns the array. */
	osign_free_signature_blobs(sigs, count);

	if (uri)
		FreePool(uri);

	return efi_status;
}
