#include "shim.h"
#include "keyless-sup.h"
#include "http-request.h"
#include "include/pe.h"

#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/opensslv.h>
#include <openssl/objects.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/asn1.h>
#include <openssl/pem.h>

BOOLEAN
osign_verify(EFI_HANDLE image_handle,CHAR16 *target_path);

#define OSIGN_MAX_SIGNATURES 8

typedef struct {
	CHAR8 *payload;
	CHAR8 *signature;
	CHAR8 *certificate;
} OSIGN_SIGNATURE;

EFI_STATUS osign_parse_pkcs7(
	char *data,
	UINTN datasize,
	PE_COFF_LOADER_IMAGE_CONTEXT ctx,
	OSIGN_SIGNATURE *out_sigs,
	UINTN max_sigs,
	UINTN *out_count);

EFI_STATUS osign_http_request(
	EFI_HANDLE image_handle,
	OSIGN_SIGNATURE *sigs,
	UINTN count);