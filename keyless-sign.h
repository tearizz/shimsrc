#include "http-request.h"
#include "include/pe.h"
#include "keyless-sup.h"
#include "shim.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

BOOLEAN
osign_verify(EFI_HANDLE image_handle, CHAR16 *target_path);

/*
 * Defensive hard limit for one image.  This is not a protocol limit: it
 * only prevents a malformed PE image from forcing an unbounded UEFI pool
 * allocation.  The parser still supports any count below this value.
 */
#ifndef OSIGN_MAX_SIGNATURE_COUNT
#define OSIGN_MAX_SIGNATURE_COUNT 256
#endif

typedef struct {
	CHAR8 *payload;
	CHAR8 *signature;
	CHAR8 *certificate;
} OSIGN_SIGNATURE;

EFI_STATUS osign_parse_pkcs7(char *data, UINTN datasize,
                             PE_COFF_LOADER_IMAGE_CONTEXT ctx,
                             OSIGN_SIGNATURE **out_sigs, UINTN *out_count);

EFI_STATUS osign_http_request(EFI_HANDLE image_handle, OSIGN_SIGNATURE *sigs,
                              UINTN count);
