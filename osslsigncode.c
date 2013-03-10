/*
   OpenSSL based Authenticode signing for PE/MSI/Java CAB files.

	 Copyright (C) 2005-2013 Per Allansson <pallansson@gmail.com>


   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

static const char *rcsid = "$Id: osslsigncode.c,v 1.4 2011/08/12 11:08:12 mfive Exp $";

/*
   Implemented with good help from:

   * Peter Gutmann's analysis of Authenticode:

	  http://www.cs.auckland.ac.nz/~pgut001/pubs/authenticode.txt

   * MS CAB SDK documentation

	  http://msdn.microsoft.com/library/default.asp?url=/library/en-us/dncabsdk/html/cabdl.asp

   * MS PE/COFF documentation

	  http://www.microsoft.com/whdc/system/platform/firmware/PECOFF.mspx

   * MS Windows Authenticode PE Signature Format

	  http://msdn.microsoft.com/en-US/windows/hardware/gg463183

	  (Although the part of how the actual checksumming is done is not
	  how it is done inside Windows. The end result is however the same
	  on all "normal" PE files.)

   * tail -c, tcpdump, mimencode & openssl asn1parse :)

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef WITH_GSF
#include <gsf/gsf-infile-msole.h>
#include <gsf/gsf-infile.h>
#include <gsf/gsf-input-stdio.h>
#include <gsf/gsf-outfile-msole.h>
#include <gsf/gsf-outfile.h>
#include <gsf/gsf-output-stdio.h>
#include <gsf/gsf-utils.h>
#endif

#include <openssl/err.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pkcs7.h>
#include <openssl/pkcs12.h>
#include <openssl/pem.h>
#include <openssl/asn1t.h>

#ifdef ENABLE_CURL
#include <curl/curl.h>
#endif

/* MS Authenticode object ids */
#define SPC_INDIRECT_DATA_OBJID	 "1.3.6.1.4.1.311.2.1.4"
#define SPC_STATEMENT_TYPE_OBJID "1.3.6.1.4.1.311.2.1.11"
#define SPC_SP_OPUS_INFO_OBJID	 "1.3.6.1.4.1.311.2.1.12"
#define SPC_INDIVIDUAL_SP_KEY_PURPOSE_OBJID "1.3.6.1.4.1.311.2.1.21"
#define SPC_COMMERCIAL_SP_KEY_PURPOSE_OBJID "1.3.6.1.4.1.311.2.1.22"
#define SPC_MS_JAVA_SOMETHING	 "1.3.6.1.4.1.311.15.1"
#define SPC_PE_IMAGE_DATA_OBJID	 "1.3.6.1.4.1.311.2.1.15"
#define SPC_CAB_DATA_OBJID		 "1.3.6.1.4.1.311.2.1.25"
#define SPC_TIME_STAMP_REQUEST_OBJID "1.3.6.1.4.1.311.3.2.1"
#define SPC_SIPINFO_OBJID		 "1.3.6.1.4.1.311.2.1.30"

#define SPC_PE_IMAGE_PAGE_HASHES_V1 "1.3.6.1.4.1.311.2.3.1" /* Page hash using SHA1 */
#define SPC_PE_IMAGE_PAGE_HASHES_V2 "1.3.6.1.4.1.311.2.3.2" /* Page hash using SHA256 */

/* 1.3.6.1.4.1.311.4... MS Crypto 2.0 stuff... */


#define WIN_CERT_REVISION_2             0x0200
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA  0x0002


/*
  ASN.1 definitions (more or less from official MS Authenticode docs)
*/

typedef struct {
	int type;
	union {
		ASN1_BMPSTRING *unicode;
		ASN1_IA5STRING *ascii;
	} value;
} SpcString;

ASN1_CHOICE(SpcString) = {
	ASN1_IMP_OPT(SpcString, value.unicode, ASN1_BMPSTRING , 0),
	ASN1_IMP_OPT(SpcString, value.ascii,   ASN1_IA5STRING,	1)
} ASN1_CHOICE_END(SpcString)

IMPLEMENT_ASN1_FUNCTIONS(SpcString)


typedef struct {
	ASN1_OCTET_STRING *classId;
	ASN1_OCTET_STRING *serializedData;
} SpcSerializedObject;

ASN1_SEQUENCE(SpcSerializedObject) = {
	ASN1_SIMPLE(SpcSerializedObject, classId, ASN1_OCTET_STRING),
	ASN1_SIMPLE(SpcSerializedObject, serializedData, ASN1_OCTET_STRING)
} ASN1_SEQUENCE_END(SpcSerializedObject)

IMPLEMENT_ASN1_FUNCTIONS(SpcSerializedObject)


typedef struct {
	int type;
	union {
		ASN1_IA5STRING *url;
		SpcSerializedObject *moniker;
		SpcString *file;
	} value;
} SpcLink;

ASN1_CHOICE(SpcLink) = {
	ASN1_IMP_OPT(SpcLink, value.url,	 ASN1_IA5STRING,	  0),
	ASN1_IMP_OPT(SpcLink, value.moniker, SpcSerializedObject, 1),
	ASN1_EXP_OPT(SpcLink, value.file,	 SpcString,			  2)
} ASN1_CHOICE_END(SpcLink)

IMPLEMENT_ASN1_FUNCTIONS(SpcLink)


typedef struct {
	SpcString *programName;
	SpcLink	  *moreInfo;
} SpcSpOpusInfo;

DECLARE_ASN1_FUNCTIONS(SpcSpOpusInfo)

ASN1_SEQUENCE(SpcSpOpusInfo) = {
	ASN1_EXP_OPT(SpcSpOpusInfo, programName, SpcString, 0),
	ASN1_EXP_OPT(SpcSpOpusInfo, moreInfo, SpcLink, 1)
} ASN1_SEQUENCE_END(SpcSpOpusInfo)

IMPLEMENT_ASN1_FUNCTIONS(SpcSpOpusInfo)


typedef struct {
	ASN1_OBJECT *type;
	ASN1_TYPE *value;
} SpcAttributeTypeAndOptionalValue;

ASN1_SEQUENCE(SpcAttributeTypeAndOptionalValue) = {
	ASN1_SIMPLE(SpcAttributeTypeAndOptionalValue, type, ASN1_OBJECT),
	ASN1_OPT(SpcAttributeTypeAndOptionalValue, value, ASN1_ANY)
} ASN1_SEQUENCE_END(SpcAttributeTypeAndOptionalValue)

IMPLEMENT_ASN1_FUNCTIONS(SpcAttributeTypeAndOptionalValue)


typedef struct {
	ASN1_OBJECT *algorithm;
	ASN1_TYPE *parameters;
} AlgorithmIdentifier;

ASN1_SEQUENCE(AlgorithmIdentifier) = {
	ASN1_SIMPLE(AlgorithmIdentifier, algorithm, ASN1_OBJECT),
	ASN1_OPT(AlgorithmIdentifier, parameters, ASN1_ANY)
} ASN1_SEQUENCE_END(AlgorithmIdentifier)

IMPLEMENT_ASN1_FUNCTIONS(AlgorithmIdentifier)

typedef struct {
	AlgorithmIdentifier *digestAlgorithm;
	ASN1_OCTET_STRING *digest;
} DigestInfo;

ASN1_SEQUENCE(DigestInfo) = {
	ASN1_SIMPLE(DigestInfo, digestAlgorithm, AlgorithmIdentifier),
	ASN1_SIMPLE(DigestInfo, digest, ASN1_OCTET_STRING)
} ASN1_SEQUENCE_END(DigestInfo)

IMPLEMENT_ASN1_FUNCTIONS(DigestInfo)

typedef struct {
	SpcAttributeTypeAndOptionalValue *data;
	DigestInfo *messageDigest;
} SpcIndirectDataContent;

ASN1_SEQUENCE(SpcIndirectDataContent) = {
	ASN1_SIMPLE(SpcIndirectDataContent, data, SpcAttributeTypeAndOptionalValue),
	ASN1_SIMPLE(SpcIndirectDataContent, messageDigest, DigestInfo)
} ASN1_SEQUENCE_END(SpcIndirectDataContent)

IMPLEMENT_ASN1_FUNCTIONS(SpcIndirectDataContent)

typedef struct {
	ASN1_BIT_STRING* flags;
	SpcLink *file;
} SpcPeImageData;

ASN1_SEQUENCE(SpcPeImageData) = {
	ASN1_SIMPLE(SpcPeImageData, flags, ASN1_BIT_STRING),
	ASN1_EXP_OPT(SpcPeImageData, file, SpcLink, 0)
} ASN1_SEQUENCE_END(SpcPeImageData)

IMPLEMENT_ASN1_FUNCTIONS(SpcPeImageData)

typedef struct {
	ASN1_INTEGER *a;
	ASN1_OCTET_STRING *string;
	ASN1_INTEGER *b;
	ASN1_INTEGER *c;
	ASN1_INTEGER *d;
	ASN1_INTEGER *e;
	ASN1_INTEGER *f;
} SpcSipinfo;

ASN1_SEQUENCE(SpcSipinfo) = {
	ASN1_SIMPLE(SpcSipinfo, a, ASN1_INTEGER),
	ASN1_SIMPLE(SpcSipinfo, string, ASN1_OCTET_STRING),
	ASN1_SIMPLE(SpcSipinfo, b, ASN1_INTEGER),
	ASN1_SIMPLE(SpcSipinfo, c, ASN1_INTEGER),
	ASN1_SIMPLE(SpcSipinfo, d, ASN1_INTEGER),
	ASN1_SIMPLE(SpcSipinfo, e, ASN1_INTEGER),
	ASN1_SIMPLE(SpcSipinfo, f, ASN1_INTEGER),
} ASN1_SEQUENCE_END(SpcSipinfo)

IMPLEMENT_ASN1_FUNCTIONS(SpcSipinfo)

#ifdef ENABLE_CURL

typedef struct {
	ASN1_OBJECT *type;
	ASN1_OCTET_STRING *signature;
} TimeStampRequestBlob;

DECLARE_ASN1_FUNCTIONS(TimeStampRequestBlob)

ASN1_SEQUENCE(TimeStampRequestBlob) = {
	ASN1_SIMPLE(TimeStampRequestBlob, type, ASN1_OBJECT),
	ASN1_EXP_OPT(TimeStampRequestBlob, signature, ASN1_OCTET_STRING, 0)
} ASN1_SEQUENCE_END(TimeStampRequestBlob)

IMPLEMENT_ASN1_FUNCTIONS(TimeStampRequestBlob)



typedef struct {
	ASN1_OBJECT *type;
	TimeStampRequestBlob *blob;
} TimeStampRequest;

DECLARE_ASN1_FUNCTIONS(TimeStampRequest)

ASN1_SEQUENCE(TimeStampRequest) = {
	ASN1_SIMPLE(TimeStampRequest, type, ASN1_OBJECT),
	ASN1_SIMPLE(TimeStampRequest, blob, TimeStampRequestBlob)
} ASN1_SEQUENCE_END(TimeStampRequest)

IMPLEMENT_ASN1_FUNCTIONS(TimeStampRequest)


/* RFC3161 Time stamping */

typedef struct {
	ASN1_INTEGER *status;
	STACK_OF(ASN1_UTF8STRING) *statusString;
	ASN1_BIT_STRING *failInfo;
} PKIStatusInfo;

ASN1_SEQUENCE(PKIStatusInfo) = {
	ASN1_SIMPLE(PKIStatusInfo, status, ASN1_INTEGER),
	ASN1_SEQUENCE_OF_OPT(PKIStatusInfo, statusString, ASN1_UTF8STRING),
	ASN1_OPT(PKIStatusInfo, failInfo, ASN1_BIT_STRING)
} ASN1_SEQUENCE_END(PKIStatusInfo)

IMPLEMENT_ASN1_FUNCTIONS(PKIStatusInfo)


typedef struct {
	PKIStatusInfo *status;
	PKCS7 *token;
} TimeStampResp;

ASN1_SEQUENCE(TimeStampResp) = {
	ASN1_SIMPLE(TimeStampResp, status, PKIStatusInfo),
	ASN1_OPT(TimeStampResp, token, PKCS7)
} ASN1_SEQUENCE_END(TimeStampResp)

IMPLEMENT_ASN1_FUNCTIONS(TimeStampResp)

typedef struct {
	AlgorithmIdentifier *digestAlgorithm;
	ASN1_OCTET_STRING *digest;
} MessageImprint;

ASN1_SEQUENCE(MessageImprint) = {
	ASN1_SIMPLE(MessageImprint, digestAlgorithm, AlgorithmIdentifier),
	ASN1_SIMPLE(MessageImprint, digest, ASN1_OCTET_STRING)
} ASN1_SEQUENCE_END(MessageImprint)

IMPLEMENT_ASN1_FUNCTIONS(MessageImprint)

typedef struct {
	ASN1_INTEGER *version;
	MessageImprint *messageImprint;
	ASN1_OBJECT *reqPolicy;
	ASN1_INTEGER *nonce;
	ASN1_BOOLEAN *certReq;
	STACK_OF(X509_EXTENSION) *extensions;
} TimeStampReq;

ASN1_SEQUENCE(TimeStampReq) = {
	ASN1_SIMPLE(TimeStampReq, version, ASN1_INTEGER),
	ASN1_SIMPLE(TimeStampReq, messageImprint, MessageImprint),
	ASN1_OPT   (TimeStampReq, reqPolicy, ASN1_OBJECT),
	ASN1_OPT   (TimeStampReq, nonce, ASN1_INTEGER),
	ASN1_SIMPLE(TimeStampReq, certReq, ASN1_BOOLEAN),
	ASN1_IMP_SEQUENCE_OF_OPT(TimeStampReq, extensions, X509_EXTENSION, 0)
} ASN1_SEQUENCE_END(TimeStampReq)

IMPLEMENT_ASN1_FUNCTIONS(TimeStampReq)

#endif /* ENABLE_CURL */


static SpcSpOpusInfo* createOpus(const char *desc, const char *url)
{
	SpcSpOpusInfo *info = SpcSpOpusInfo_new();

	if (desc) {
		info->programName = SpcString_new();
		info->programName->type = 1;
		info->programName->value.ascii = M_ASN1_IA5STRING_new();
		ASN1_STRING_set((ASN1_STRING *)info->programName->value.ascii,
						(const unsigned char*)desc, strlen(desc));
	}

	if (url) {
		info->moreInfo = SpcLink_new();
		info->moreInfo->type = 0;
		info->moreInfo->value.url = M_ASN1_IA5STRING_new();
		ASN1_STRING_set((ASN1_STRING *)info->moreInfo->value.url,
						(const unsigned char*)url, strlen(url));
	}

	return info;
}

#ifdef ENABLE_CURL

static int blob_has_nl = 0;
static size_t curl_write( void *ptr, size_t sz, size_t nmemb, void *stream)
{
	if (sz*nmemb > 0 && !blob_has_nl) {
		if (memchr(ptr, '\n', sz*nmemb))
			blob_has_nl = 1;
	}
	return BIO_write((BIO*)stream, ptr, sz*nmemb);
}

/*
  A timestamp request looks like this:

  POST <someurl> HTTP/1.1
  Content-Type: application/octet-stream
  Content-Length: ...
  Accept: application/octet-stream
  User-Agent: Transport
  Host: ...
  Cache-Control: no-cache

  <base64encoded blob>


  .. and the blob has the following ASN1 structure:

  0:d=0	 hl=4 l= 291 cons: SEQUENCE
  4:d=1	 hl=2 l=  10 prim:	OBJECT			  :1.3.6.1.4.1.311.3.2.1
  16:d=1  hl=4 l= 275 cons:	 SEQUENCE
  20:d=2  hl=2 l=	9 prim:	  OBJECT			:pkcs7-data
  31:d=2  hl=4 l= 260 cons:	  cont [ 0 ]
  35:d=3  hl=4 l= 256 prim:	   OCTET STRING
  <signature>



  .. and it returns a base64 encoded PKCS#7 structure.

*/

static int add_timestamp(PKCS7 *sig, char *url, char *proxy, int rfc3161, const EVP_MD *md, unsigned char *mdbuf)
{
	CURL *curl;
	struct curl_slist *slist = NULL;
	CURLcode c;
	BIO *bout, *bin, *b64;
	u_char *p;
	int len;
	PKCS7_SIGNER_INFO *si =
		sk_PKCS7_SIGNER_INFO_value
		(sig->d.sign->signer_info, 0);

	if (!url) return -1;

	curl = curl_easy_init();

	if (proxy) {
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
		if (!strncmp("http:", proxy, 5))
			curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
		if (!strncmp("socks:", proxy, 6))
			curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
/*	  curl_easy_setopt(curl, CURLOPT_VERBOSE, 42);	*/

	if (rfc3161) {
		slist = curl_slist_append(slist, "Content-Type: application/timestamp-query");
		slist = curl_slist_append(slist, "Accept: application/timestamp-reply");
	} else {
		slist = curl_slist_append(slist, "Content-Type: application/octet-stream");
		slist = curl_slist_append(slist, "Accept: application/octet-stream");
	}
	slist = curl_slist_append(slist, "User-Agent: Transport");
	slist = curl_slist_append(slist, "Cache-Control: no-cache");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

	if (rfc3161) {
		TimeStampReq *req = TimeStampReq_new();
		req->version = ASN1_INTEGER_new();
		ASN1_INTEGER_set(req->version, 1);
		req->messageImprint = MessageImprint_new();
		req->messageImprint->digestAlgorithm = AlgorithmIdentifier_new();
		req->messageImprint->digestAlgorithm->algorithm = OBJ_nid2obj(EVP_MD_nid(md));
		req->messageImprint->digestAlgorithm->parameters = ASN1_TYPE_new();
		req->messageImprint->digestAlgorithm->parameters->type = V_ASN1_NULL;
		req->messageImprint->digest = M_ASN1_OCTET_STRING_new();
		M_ASN1_OCTET_STRING_set(req->messageImprint->digest, mdbuf, EVP_MD_size(md));
		int yes = 1;
		req->certReq = &yes;
		len = i2d_TimeStampReq(req, NULL);
		p = OPENSSL_malloc(len);
		len = i2d_TimeStampReq(req, &p);
		p -= len;

		req->certReq = NULL;
		TimeStampReq_free(req);
	} else {
		TimeStampRequest *req = TimeStampRequest_new();
		req->type = OBJ_txt2obj(SPC_TIME_STAMP_REQUEST_OBJID, 1);
		req->blob = TimeStampRequestBlob_new();
		req->blob->type = OBJ_nid2obj(NID_pkcs7_data);
		req->blob->signature = si->enc_digest;

		len = i2d_TimeStampRequest(req, NULL);
		p = OPENSSL_malloc(len);
		len = i2d_TimeStampRequest(req, &p);
		p -= len;

		TimeStampRequest_free(req);
	}

	bout = BIO_new(BIO_s_mem());
	if (!rfc3161) {
		b64 = BIO_new(BIO_f_base64());
		bout = BIO_push(b64, bout);
	}
	BIO_write(bout, p, len);
	(void)BIO_flush(bout);
	OPENSSL_free(p);

	len = BIO_get_mem_data(bout, &p);

	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, len);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char*)p);

	bin = BIO_new(BIO_s_mem());
	BIO_set_mem_eof_return(bin, 0);
	curl_easy_setopt(curl, CURLOPT_POST, 1);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, bin);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);

	c = curl_easy_perform(curl);

	curl_slist_free_all(slist);
	BIO_free_all(bout);

	if (c) {
		BIO_free_all(bin);
		fprintf(stderr, "CURL failure: %s\n", curl_easy_strerror(c));
	} else {
		PKCS7 *p7;
		int i;
		PKCS7_SIGNER_INFO *info;
		ASN1_STRING *astr;

		(void)BIO_flush(bin);

		if (rfc3161) {
			TimeStampResp *reply;
			(void)BIO_flush(bin);
			reply = ASN1_item_d2i_bio(ASN1_ITEM_rptr(TimeStampResp), bin, NULL);
			BIO_free_all(bin);
			if (!reply) {
				fprintf(stderr, "Failed to convert timestamp reply\n");
				ERR_print_errors_fp(stderr);
				return -1;
			}
			if (ASN1_INTEGER_get(reply->status->status) != 0) {
				fprintf(stderr, "Timestamping failed: %ld\n", ASN1_INTEGER_get(reply->status->status));
				TimeStampResp_free(reply);
				return -1;
			}
			p7 = PKCS7_dup(reply->token);
			TimeStampResp_free(reply);
		} else {
			BIO* b64_bin;
			b64 = BIO_new(BIO_f_base64());
			if (!blob_has_nl)
				BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
			b64_bin = BIO_push(b64, bin);
			p7 = d2i_PKCS7_bio(b64_bin, NULL);
			if (p7 == NULL) {
				BIO_free_all(b64_bin);
				fprintf(stderr, "Failed to convert timestamp reply\n");
				ERR_print_errors_fp(stderr);
				return -1;
			}
			BIO_free_all(b64_bin);
		}

		for(i = sk_X509_num(p7->d.sign->cert)-1; i>=0; i--)
			PKCS7_add_certificate(sig, sk_X509_value(p7->d.sign->cert, i));

		info = sk_PKCS7_SIGNER_INFO_value(p7->d.sign->signer_info, 0);
		if (((len = i2d_PKCS7_SIGNER_INFO(info, NULL)) <= 0) ||
			(p = OPENSSL_malloc(len)) == NULL) {
			fprintf(stderr, "Failed to convert signer info: %d\n", len);
			ERR_print_errors_fp(stderr);
			PKCS7_free(p7);
			return -1;
		}
		len = i2d_PKCS7_SIGNER_INFO(info, &p);
		p -= len;
		astr = ASN1_STRING_new();
		ASN1_STRING_set(astr, p, len);
		PKCS7_add_attribute
			(si, NID_pkcs9_countersignature,
			 V_ASN1_SEQUENCE, astr);

		PKCS7_free(p7);
	}

	curl_easy_cleanup(curl);

	return (int)c;
}

static int add_timestamp_authenticode(PKCS7 *sig, char *url, char *proxy)
{
	return add_timestamp(sig, url, proxy, 0, NULL, NULL);
}

static int add_timestamp_rfc3161(PKCS7 *sig, char *url, char *proxy, const EVP_MD *md, unsigned char *mdbuf)
{
	return add_timestamp(sig, url, proxy, 1, md, mdbuf);
}

#endif /* ENABLE_CURL */


static void usage(const char *argv0)
{
	fprintf(stderr,
			"Usage: %s\n\n\t[ --version | -v ]\n\n"
			"\t[ sign ]\n"
			"\t\t( -spc <spcfile> -key <keyfile> | -pkcs12 <pkcs12file> "
#if OPENSSL_VERSION_NUMBER > 0x10000000
			"| -spc <spcfile> -pvk <pvkfile> "
#endif
			")\n"
			"\t\t[ -pass <keypass> ]\n"
			"\t\t[ -h {md5,sha1,sha2} ]\n"
			"\t\t[ -n <desc> ] [ -i <url> ] [ -jp <level> ] [ -comm ]\n"
#ifdef ENABLE_CURL
			"\t\t[ -t <timestampurl> [ -p <proxy> ]]\n"
			"\t\t[ -ts <timestampurl> [ -p <proxy> ]]\n"
#endif
			"\t\t[ -in ] <infile> [-out ] <outfile>\n\n"
			"\textract-signature [ -in ] <infile> [ -out ] <outfile>\n\n"
			"\tremove-signature [ -in ] <infile> [ -out ] <outfile>\n\n"
			"\tverify [ -in ] <infile>\n\n"
			"",
			argv0);
	exit(-1);
}

#define DO_EXIT_0(x)	{ fputs(x, stderr); goto err_cleanup; }
#define DO_EXIT_1(x, y) { fprintf(stderr, x, y); goto err_cleanup; }
#define DO_EXIT_2(x, y, z) { fprintf(stderr, x, y, z); goto err_cleanup; }

#define GET_UINT16_LE(p) (((u_char*)(p))[0] | (((u_char*)(p))[1]<<8))

#define GET_UINT32_LE(p) (((u_char*)(p))[0] | (((u_char*)(p))[1]<<8) |	\
						  (((u_char*)(p))[2]<<16) | (((u_char*)(p))[3]<<24))

#define PUT_UINT16_LE(i,p)						\
	((u_char*)(p))[0] = (i) & 0xff;				\
	((u_char*)(p))[1] = ((i)>>8) & 0xff

#define PUT_UINT32_LE(i,p)						\
	((u_char*)(p))[0] = (i) & 0xff;				\
	((u_char*)(p))[1] = ((i)>>8) & 0xff;		\
	((u_char*)(p))[2] = ((i)>>16) & 0xff;		\
	((u_char*)(p))[3] = ((i)>>24) & 0xff


#ifdef HACK_OPENSSL
ASN1_TYPE *PKCS7_get_signed_attribute(PKCS7_SIGNER_INFO *si, int nid)
/* ARGSUSED */
{
	/* Ehhhm. Hack. The PKCS7 sign method adds NID_pkcs9_signingTime if
	   it isn't there. But we don't want it since M$ barfs on it.
	   Sooooo... let's pretend it's here. */
	return (ASN1_TYPE*)0xdeadbeef;
}
#endif

typedef enum {
	FILE_TYPE_CAB,
	FILE_TYPE_PE,
	FILE_TYPE_MSI,
} file_type_t;

typedef enum {
	CMD_SIGN,
	CMD_EXTRACT,
	CMD_REMOVE,
	CMD_VERIFY,
} cmd_type_t;

static void get_indirect_data_blob(u_char **blob, int *len, const EVP_MD *md, file_type_t type)
{
	static const unsigned char obsolete[] = {
		0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x4f, 0x00, 0x62,
		0x00, 0x73, 0x00, 0x6f, 0x00, 0x6c, 0x00, 0x65, 0x00, 0x74,
		0x00, 0x65, 0x00, 0x3e, 0x00, 0x3e, 0x00, 0x3e
	};
	static const unsigned char msistr[] = {
		0xf1, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
		0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
	};

	u_char *p;
	int hashlen, l;
	void *hash;
	SpcLink *link;
	SpcIndirectDataContent *idc = SpcIndirectDataContent_new();
	idc->data = SpcAttributeTypeAndOptionalValue_new();

	link = SpcLink_new();
	link->type = 2;
	link->value.file = SpcString_new();
	link->value.file->type = 0;
	link->value.file->value.unicode = ASN1_BMPSTRING_new();
	ASN1_STRING_set(link->value.file->value.unicode, obsolete, sizeof(obsolete));

	idc->data->value = ASN1_TYPE_new();
	idc->data->value->type = V_ASN1_SEQUENCE;
	idc->data->value->value.sequence = ASN1_STRING_new();
	if (type == FILE_TYPE_CAB) {
		l = i2d_SpcLink(link, NULL);
		p = OPENSSL_malloc(l);
		i2d_SpcLink(link, &p);
		p -= l;
		idc->data->type = OBJ_txt2obj(SPC_CAB_DATA_OBJID, 1);
	} else if (type == FILE_TYPE_PE) {
		SpcPeImageData *pid = SpcPeImageData_new();
		pid->flags = ASN1_BIT_STRING_new();
		ASN1_BIT_STRING_set(pid->flags, (unsigned char*)"0", 0);
		pid->file = link;
		l = i2d_SpcPeImageData(pid, NULL);
		p = OPENSSL_malloc(l);
		i2d_SpcPeImageData(pid, &p);
		p -= l;
		idc->data->type = OBJ_txt2obj(SPC_PE_IMAGE_DATA_OBJID, 1);
	} else if (type == FILE_TYPE_MSI) {
		SpcSipinfo *si = SpcSipinfo_new();

		si->a = ASN1_INTEGER_new();
		ASN1_INTEGER_set(si->a, 1);
		si->string = M_ASN1_OCTET_STRING_new();
		M_ASN1_OCTET_STRING_set(si->string, msistr, sizeof(msistr));
		si->b = ASN1_INTEGER_new();
		si->c = ASN1_INTEGER_new();
		si->d = ASN1_INTEGER_new();
		si->e = ASN1_INTEGER_new();
		si->f = ASN1_INTEGER_new();
		l = i2d_SpcSipinfo(si, NULL);
		p = OPENSSL_malloc(l);
		i2d_SpcSipinfo(si, &p);
		p -= l;
		idc->data->type = OBJ_txt2obj(SPC_SIPINFO_OBJID, 1);
	} else {
		fprintf(stderr, "Unexpected file type: %d\n", type);
		exit(1);
	}

	idc->data->value->value.sequence->data = p;
	idc->data->value->value.sequence->length = l;
	idc->messageDigest = DigestInfo_new();
	idc->messageDigest->digestAlgorithm = AlgorithmIdentifier_new();
	idc->messageDigest->digestAlgorithm->algorithm = OBJ_nid2obj(EVP_MD_nid(md));
	idc->messageDigest->digestAlgorithm->parameters = ASN1_TYPE_new();
	idc->messageDigest->digestAlgorithm->parameters->type = V_ASN1_NULL;
	idc->messageDigest->digest = M_ASN1_OCTET_STRING_new();

	hashlen = EVP_MD_size(md);
	hash = OPENSSL_malloc(hashlen);
	memset(hash, 0, hashlen);
	M_ASN1_OCTET_STRING_set(idc->messageDigest->digest, hash, hashlen);

	*len  = i2d_SpcIndirectDataContent(idc, NULL);
	*blob = OPENSSL_malloc(*len);
	p = *blob;
	i2d_SpcIndirectDataContent(idc, &p);
}

static unsigned int calc_pe_checksum(BIO *bio, unsigned int peheader)
{
	unsigned int checkSum = 0;
	unsigned short	val;
	unsigned int size = 0;

	/* recalc checksum. */
	(void)BIO_seek(bio, 0);
	while (BIO_read(bio, &val, 2) == 2) {
		if (size == peheader + 88 || size == peheader + 90)
			val = 0;
		checkSum += val;
		checkSum = 0xffff & (checkSum + (checkSum >> 0x10));
		size += 2;
	}

	checkSum = 0xffff & (checkSum + (checkSum >> 0x10));
	checkSum += size;

	return checkSum;
}

static void recalc_pe_checksum(BIO *bio, unsigned int peheader)
{
	unsigned int checkSum = calc_pe_checksum(bio, peheader);
	char buf[4];

	/* write back checksum. */
	(void)BIO_seek(bio, peheader + 88);
	PUT_UINT32_LE(checkSum, buf);
	BIO_write(bio, buf, 4);
}

#ifdef WITH_GSF
static gint msi_base64_decode(gint x)
{
	if (x < 10)
		return x + '0';
	if (x < (10 + 26))
		return x - 10 + 'A';
	if (x < (10 + 26 + 26))
		return x - 10 - 26 + 'a';
	if (x == (10 + 26 + 26))
		return '.';
	return 1;
}

static void msi_decode(const guint8 *in, gchar *out)
{
	guint count = 0;
	guint8 *q = (guint8 *)out;

	/* utf-8 encoding of 0x4840 */
	if (in[0] == 0xe4 && in[1] == 0xa1 && in[2] == 0x80)
		in += 3;

	while (*in) {
		guint8 ch = *in;
		if ((ch == 0xe3 && in[1] >= 0xa0) || (ch == 0xe4 && in[1] < 0xa0)) {
			*q++ = msi_base64_decode(in[2] & 0x7f);
			*q++ = msi_base64_decode(in[1] ^ 0xa0);
			in += 3;
			count += 2;
			continue;
		}
		if (ch == 0xe4 && in[1] == 0xa0) {
			*q++ = msi_base64_decode(in[2] & 0x7f);
			in += 3;
			count++;
			continue;
		}
		*q++ = *in++;
		if (ch >= 0xc1)
			*q++ = *in++;
		if (ch >= 0xe0)
			*q++ = *in++;
		if (ch >= 0xf0)
			*q++ = *in++;
		count++;
	}
	*q = 0;
}

/*
 * Sorry if this code looks a bit silly, but that seems
 * to be the best solution so far...
 */
static gint msi_cmp(gpointer a, gpointer b)
{
	gchar *pa = (gchar*)g_utf8_to_utf16(a, -1, NULL, NULL, NULL);
	gchar *pb = (gchar*)g_utf8_to_utf16(b, -1, NULL, NULL, NULL);
	gint diff;

	diff = memcmp(pa, pb, MIN(strlen(pa), strlen(pb)));
	/* apparently the longer wins */
	if (diff == 0)
		return strlen(pa) > strlen(pb) ? 1 : -1;
	g_free(pa);
	g_free(pb);

	return diff;
}
#endif

static void tohex(const unsigned char *v, unsigned char *b, int len)
{
	int i;
	for(i=0; i<len; i++)
		sprintf(b+i*2, "%02X", v[i]);
	b[i*2] = 0x00;
}


static void calc_pe_digest(BIO *bio, const EVP_MD *md, unsigned char *mdbuf,
						   unsigned int peheader, int pe32plus, unsigned int fileend)
{
	static unsigned char bfb[16*1024*1024];
	EVP_MD_CTX mdctx;

	EVP_MD_CTX_init(&mdctx);
	EVP_DigestInit(&mdctx, md);

	memset(mdbuf, 0, EVP_MAX_MD_SIZE);

	BIO_seek(bio, 0);
	BIO_read(bio, bfb, peheader + 88);
	EVP_DigestUpdate(&mdctx, bfb, peheader + 88);
	BIO_read(bio, bfb, 4);
	BIO_read(bio, bfb, 60+pe32plus*16);
	EVP_DigestUpdate(&mdctx, bfb, 60+pe32plus*16);
	BIO_read(bio, bfb, 8);

	unsigned int n = peheader + 88 + 4 + 60+pe32plus*16 + 8;
	while (n < fileend) {
		int want = fileend - n;
		if (want > sizeof(bfb))
			want = sizeof(bfb);
		int l = BIO_read(bio, bfb, want);
		if (l <= 0)
			break;
		EVP_DigestUpdate(&mdctx, bfb, l);
		n += l;
	}

	EVP_DigestFinal(&mdctx, mdbuf, NULL);
}


static unsigned int asn1_simple_hdr_len(const unsigned char *p, unsigned int len) {
	if (len <= 2 || p[0] > 0x31)
		return 0;
	return (p[1]&0x80) ? (2 + p[1]&0x7f) : 2;
}

static const unsigned char classid_page_hash[] = {
	0xA6, 0xB5, 0x86, 0xD5, 0xB4, 0xA1, 0x24, 0x66,
	0xAE, 0x05, 0xA2, 0x17, 0xDA, 0x8E, 0x60, 0xD6
};

static void	extract_page_hash (SpcAttributeTypeAndOptionalValue *obj, unsigned char **ph, unsigned int *phlen, int *phtype)
{
	*phlen = 0;

	const unsigned char *blob = obj->value->value.sequence->data;
	SpcPeImageData *id = d2i_SpcPeImageData(NULL, &blob, obj->value->value.sequence->length);
	if (id == NULL)
		return;

	if (id->file->type != 1) {
		SpcPeImageData_free(id);
		return;
	}

	SpcSerializedObject *so = id->file->value.moniker;
	if (so->classId->length != sizeof(classid_page_hash) ||
		memcmp(so->classId->data, classid_page_hash, sizeof (classid_page_hash))) {
		SpcPeImageData_free(id);
		return;
	}

	/* skip ASN.1 SET hdr */
	unsigned int l = asn1_simple_hdr_len(so->serializedData->data, so->serializedData->length);
	blob = so->serializedData->data + l;
	obj = d2i_SpcAttributeTypeAndOptionalValue(NULL, &blob, so->serializedData->length - l);
	SpcPeImageData_free(id);
	if (!obj)
		return;

	char buf[128];
	*phtype = 0;
	buf[0] = 0x00;
	OBJ_obj2txt(buf, sizeof(buf), obj->type, 1);
	if (!strcmp(buf, SPC_PE_IMAGE_PAGE_HASHES_V1)) {
		*phtype = NID_sha1;
	} else if (!strcmp(buf, SPC_PE_IMAGE_PAGE_HASHES_V2)) {
		*phtype = NID_sha256;
	} else {
		SpcAttributeTypeAndOptionalValue_free(obj);
		return;
	}

	/* Skip ASN.1 SET hdr */
	unsigned int l2 = asn1_simple_hdr_len(obj->value->value.sequence->data, obj->value->value.sequence->length);
	/* Skip ASN.1 OCTET STRING hdr */
	l =  asn1_simple_hdr_len(obj->value->value.sequence->data + l2, obj->value->value.sequence->length - l2);
	l += l2;
	*phlen = obj->value->value.sequence->length - l;
	*ph = malloc(*phlen);
	memcpy(*ph, obj->value->value.sequence->data + l, *phlen);
	SpcAttributeTypeAndOptionalValue_free(obj);
}

static int verify_pe_file(char *indata, unsigned int peheader, int pe32plus,
						  unsigned int sigpos, unsigned int siglen)
{
	int ret = 0;
	unsigned int pe_checksum = GET_UINT32_LE(indata + peheader + 88);
	printf("Current PE checksum   : %08X\n", pe_checksum);

	BIO *bio = BIO_new_mem_buf(indata, sigpos + siglen);
	unsigned int real_pe_checksum = calc_pe_checksum(bio, peheader);
	BIO_free(bio);
	if (pe_checksum && pe_checksum != real_pe_checksum)
		ret = 1;
	printf("Calculated PE checksum: %08X%s\n\n", real_pe_checksum,
		   ret ? "     MISMATCH!!!!" : "");
	if (siglen == 0) {
		printf("No signature found.\n\n");
		return ret;
	}

	int mdtype = -1, phtype = -1;
	unsigned char mdbuf[EVP_MAX_MD_SIZE];
	unsigned char cmdbuf[EVP_MAX_MD_SIZE];
	unsigned int pos = 0;
	unsigned char hexbuf[EVP_MAX_MD_SIZE*2+1];
	unsigned char *ph = NULL;
	unsigned int phlen = 0;
	PKCS7 *p7 = NULL;

	while (pos < siglen && mdtype == -1) {
		unsigned int l = GET_UINT32_LE(indata + sigpos + pos);
		unsigned short certrev  = GET_UINT16_LE(indata + sigpos + pos + 4);
		unsigned short certtype = GET_UINT16_LE(indata + sigpos + pos + 6);
		if (certrev == WIN_CERT_REVISION_2 && certtype == WIN_CERT_TYPE_PKCS_SIGNED_DATA) {
			const unsigned char *blob = indata + sigpos + pos + 8;
			p7 = d2i_PKCS7(NULL, &blob, l - 8);
			if (p7 && PKCS7_type_is_signed(p7) &&
				!OBJ_cmp(p7->d.sign->contents->type, OBJ_txt2obj(SPC_INDIRECT_DATA_OBJID, 1)) &&
				p7->d.sign->contents->d.other->type == V_ASN1_SEQUENCE) {

				ASN1_STRING *astr = p7->d.sign->contents->d.other->value.sequence;
				const unsigned char *p = astr->data;
				SpcIndirectDataContent *idc = d2i_SpcIndirectDataContent(NULL, &p, astr->length);
				if (idc) {
					extract_page_hash (idc->data, &ph, &phlen, &phtype);
					if (idc->messageDigest && idc->messageDigest->digest && idc->messageDigest->digestAlgorithm) {
						mdtype = OBJ_obj2nid(idc->messageDigest->digestAlgorithm->algorithm);
						memcpy(mdbuf, idc->messageDigest->digest->data, idc->messageDigest->digest->length);
					}
					SpcIndirectDataContent_free(idc);
				}
			}
			if (p7 && mdtype == -1) {
				PKCS7_free(p7);
				p7 = NULL;
			}
		}
		if (l%8)
			l += (8 - l%8);
		pos += l;
	}

	if (mdtype == -1) {
		printf("Failed to extract current message digest\n\n");
		return;
	}

	printf("Message digest algorithm  : %s\n", OBJ_nid2sn(mdtype));

	const EVP_MD *md = EVP_get_digestbynid(mdtype);
	tohex(mdbuf, hexbuf, EVP_MD_size(md));
	printf("Current message digest    : %s\n", hexbuf);

	bio = BIO_new_mem_buf(indata, sigpos + siglen);
	calc_pe_digest(bio, md, cmdbuf, peheader, pe32plus, sigpos);
	BIO_free(bio);
	tohex(cmdbuf, hexbuf, EVP_MD_size(md));
	int mdok = !memcmp(mdbuf, cmdbuf, EVP_MD_size(md));
	if (!mdok) ret = 1;
	printf("Calculated message digest : %s%s\n\n", hexbuf, mdok?"":"    MISMATCH!!!");

	if (phlen > 0) {
		printf("Page hash algorithm: %s\n", OBJ_nid2sn(phtype));
		tohex(ph, hexbuf, (phlen < 32) ? phlen : 32);
		printf("Page hash          : %s ...\n\n", hexbuf);
		free(ph);
	}

	int seqhdrlen = asn1_simple_hdr_len(p7->d.sign->contents->d.other->value.sequence->data,
										p7->d.sign->contents->d.other->value.sequence->length);
	bio = BIO_new_mem_buf(p7->d.sign->contents->d.other->value.sequence->data + seqhdrlen,
						  p7->d.sign->contents->d.other->value.sequence->length - seqhdrlen);
	X509_STORE *store = X509_STORE_new();
	int verok = PKCS7_verify(p7, p7->d.sign->cert, store, bio, NULL, PKCS7_NOVERIFY);
	BIO_free(bio);
	/* XXX: add more checks here (attributes, pagehash, timestamp, etc) */
	printf("Signature verification: %s\n\n", verok ? "ok" : "failed");
	if (!verok) {
		ERR_print_errors_fp(stdout);
		ret = 1;
	}

	int i;
	STACK_OF(X509) *signers = PKCS7_get0_signers(p7, NULL, 0);
	printf("Number of signers: %d\n", sk_X509_num(signers));
	for (i=0; i<sk_X509_num(signers); i++) {
		X509 *cert = sk_X509_value(signers, i);
		char *subject = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
		char *issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
		printf("\tSigner #%d:\n\t\tSubject: %s\n\t\tIssuer : %s\n", i, subject, issuer);
		OPENSSL_free(subject);
		OPENSSL_free(issuer);
	}
	sk_X509_free(signers);

	printf("\nNumber of certificates: %d\n", sk_X509_num(p7->d.sign->cert));
	for (i=0; i<sk_X509_num(p7->d.sign->cert); i++) {
		X509 *cert = sk_X509_value(p7->d.sign->cert, i);
		char *subject = X509_NAME_oneline(X509_get_subject_name(cert), NULL, 0);
		char *issuer = X509_NAME_oneline(X509_get_issuer_name(cert), NULL, 0);
		printf("\tCert #%d:\n\t\tSubject: %s\n\t\tIssuer : %s\n", i, subject, issuer);
		OPENSSL_free(subject);
		OPENSSL_free(issuer);
    }

	X509_STORE_free(store);
	PKCS7_free(p7);

	printf("\n");

	return ret;
}


int main(int argc, char **argv)
{
	BIO *btmp, *sigdata, *hash, *outdata;
	PKCS12 *p12;
	PKCS7 *p7, *sig;
	X509 *cert = NULL;
	STACK_OF(X509) *certs = NULL;
	EVP_PKEY *pkey;
	PKCS7_SIGNER_INFO *si;
	ASN1_TYPE dummy;
	ASN1_STRING *astr;
	const EVP_MD *md;

	const char *argv0 = argv[0];
	static char buf[64*1024];
	char *spcfile, *keyfile, *pkcs12file, *infile, *outfile, *desc, *url, *indata;
#if OPENSSL_VERSION_NUMBER > 0x10000000
	char *pvkfile = NULL;
#endif
	char *pass = "";
#ifdef ENABLE_CURL
	char *turl = NULL, *proxy = NULL, *tsurl = NULL;
#endif
	u_char *p;
	int ret = 0, i, len = 0, jp = -1, fd = -1, pe32plus = 0, comm = 0;
	unsigned int tmp, peheader = 0, padlen;
	off_t fileend;
	file_type_t type;
	cmd_type_t cmd = CMD_SIGN;
	struct stat st;
	char *failarg = NULL;

	static u_char purpose_ind[] = {
		0x30, 0x0c,
		0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x15
	};

	static u_char purpose_comm[] = {
		0x30, 0x0c,
		0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x16
	};

	static u_char msi_signature[] = {
		0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1
	};

#ifdef WITH_GSF
	GsfOutfile *outole = NULL;
	GsfOutput *sink = NULL;
	gsf_init();
#endif

	/* Set up OpenSSL */
	ERR_load_crypto_strings();
	OPENSSL_add_all_algorithms_conf();

	md = EVP_sha1();
	spcfile = keyfile = pkcs12file = infile = outfile = desc = url = NULL;
	hash = outdata = NULL;

	if (argc > 1) {
		if (!strcmp(argv[1], "sign")) {
			cmd = CMD_SIGN;
			argv++;
			argc--;
		} else if (!strcmp(argv[1], "extract-signature")) {
			cmd = CMD_EXTRACT;
			argv++;
			argc--;
		} else if (!strcmp(argv[1], "remove-signature")) {
			cmd = CMD_REMOVE;
			argv++;
			argc--;
		} else if (!strcmp(argv[1], "verify")) {
			cmd = CMD_VERIFY;
			argv++;
			argc--;
		}
	}

	for (argc--,argv++; argc >= 1; argc--,argv++) {
		if (!strcmp(*argv, "-in")) {
			if (--argc < 1) usage(argv0);
			infile = *(++argv);
		} else if (!strcmp(*argv, "-out")) {
			if (--argc < 1) usage(argv0);
			outfile = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-spc")) {
			if (--argc < 1) usage(argv0);
			spcfile = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-key")) {
			if (--argc < 1) usage(argv0);
			keyfile = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-pkcs12")) {
			if (--argc < 1) usage(argv0);
			pkcs12file = *(++argv);
#if OPENSSL_VERSION_NUMBER > 0x10000000
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-pvk")) {
			if (--argc < 1) usage(argv0);
			pvkfile = *(++argv);
#endif
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-pass")) {
			if (--argc < 1) usage(argv0);
			pass = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-comm")) {
			comm = 1;
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-n")) {
			if (--argc < 1) usage(argv0);
			desc = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-h")) {
			if (--argc < 1) usage(argv0);
			++argv;
			if (!strcmp(*argv, "md5")) {
				md = EVP_md5();
			} else if (!strcmp(*argv, "sha1")) {
				md = EVP_sha1();
			} else if (!strcmp(*argv, "sha2")) {
				md = EVP_sha256();
			} else {
				usage(argv0);
			}
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-i")) {
			if (--argc < 1) usage(argv0);
			url = *(++argv);
#ifdef ENABLE_CURL
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-t")) {
			if (--argc < 1) usage(argv0);
			turl = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-ts")) {
			if (--argc < 1) usage(argv0);
			tsurl = *(++argv);
		} else if ((cmd == CMD_SIGN) && !strcmp(*argv, "-p")) {
			if (--argc < 1) usage(argv0);
			proxy = *(++argv);
#endif
		} else if (!strcmp(*argv, "-v") || !strcmp(*argv, "--version")) {
			printf(PACKAGE_STRING ", using:\n\t%s\n\t%s\n",
				   SSLeay_version(SSLEAY_VERSION),
#ifdef ENABLE_CURL
				   curl_version()
#else
				   "no libcurl available"
#endif
			   );
			printf(
#ifdef WITH_GSF
				   "\tlibgsf %d.%d.%d\n",
				   libgsf_major_version,
				   libgsf_minor_version,
				   libgsf_micro_version
#else
				   "\tno libgsf available\n"
#endif
				);
			printf("\nPlease send bug-reports to "
				   PACKAGE_BUGREPORT
				   "\n\n");

		} else if (!strcmp(*argv, "-jp")) {
			char *ap;
			if (--argc < 1) usage(argv0);
			ap = *(++argv);
			for (i=0; ap[i]; i++) ap[i] = tolower((int)ap[i]);
			if (!strcmp(ap, "low")) {
				jp = 0;
			} else if (!strcmp(ap, "medium")) {
				jp = 1;
			} else if (!strcmp(ap, "high")) {
				jp = 2;
			}
			if (jp != 0) usage(argv0); /* XXX */
		} else {
			failarg = *argv;
			break;
		}
	}

	if (!infile && argc > 0) {
		infile = *(argv++);
		argc--;
	}

	if (cmd != CMD_VERIFY && (!outfile && argc > 0)) {
		if (!strcmp(*argv, "-out")) {
			argv++;
			argc--;
		}
		if (argc > 0) {
			outfile = *(argv++);
			argc--;
		}
	}

	if (argc > 0 || (turl && tsurl) || !infile ||
		(cmd != CMD_VERIFY && !outfile) ||
		(cmd == CMD_SIGN && !((spcfile && keyfile) || pkcs12file
#if OPENSSL_VERSION_NUMBER > 0x10000000
							  || (spcfile && pvkfile)
#endif
			))) {
		if (failarg)
			fprintf(stderr, "Unknown option: %s\n", failarg);
		usage(argv0);
	}

	if (cmd == CMD_SIGN) {
		/* Read certificate and key */
		if (pkcs12file != NULL) {
			if ((btmp = BIO_new_file(pkcs12file, "rb")) == NULL ||
				(p12 = d2i_PKCS12_bio(btmp, NULL)) == NULL)
				DO_EXIT_1("Failed to read PKCS#12 file: %s\n", pkcs12file);
			BIO_free(btmp);
			if (!PKCS12_parse(p12, pass, &pkey, &cert, &certs))
				DO_EXIT_1("Failed to parse PKCS#12 file: %s (Wrong password?)\n", pkcs12file);
			PKCS12_free(p12);
#if OPENSSL_VERSION_NUMBER > 0x10000000
		} else if (pvkfile != NULL) {
			if ((btmp = BIO_new_file(spcfile, "rb")) == NULL ||
				(p7 = d2i_PKCS7_bio(btmp, NULL)) == NULL)
				DO_EXIT_1("Failed to read DER-encoded spc file: %s\n", spcfile);
			BIO_free(btmp);
			if ((btmp = BIO_new_file(pvkfile, "rb")) == NULL ||
				( (pkey = b2i_PVK_bio(btmp, NULL, NULL)) == NULL &&
				  (pkey = b2i_PVK_bio(btmp, NULL, pass)) == NULL))
				DO_EXIT_1("Failed to read PVK file: %s\n", pvkfile);
			BIO_free(btmp);
#endif
		} else {
			if ((btmp = BIO_new_file(spcfile, "rb")) == NULL ||
				(p7 = d2i_PKCS7_bio(btmp, NULL)) == NULL)
				DO_EXIT_1("Failed to read DER-encoded spc file: %s\n", spcfile);
			BIO_free(btmp);

			if ((btmp = BIO_new_file(keyfile, "rb")) == NULL ||
				( (pkey = d2i_PrivateKey_bio(btmp, NULL)) == NULL &&
				  (pkey = PEM_read_bio_PrivateKey(btmp, NULL, NULL, pass)) == NULL &&
				  (pkey = PEM_read_bio_PrivateKey(btmp, NULL, NULL, NULL)) == NULL))
			DO_EXIT_1("Failed to read private key file: %s (Wrong password?)\n", keyfile);
			BIO_free(btmp);
			certs = p7->d.sign->cert;
		}
	}

	/* Check if indata is cab or pe */
	if (stat(infile, &st))
		DO_EXIT_1("Failed to open file: %s\n", infile);

	fileend = st.st_size;

	if (st.st_size < 4)
		DO_EXIT_1("Unrecognized file type - file is too short: %s\n", infile);

	if ((fd = open(infile, O_RDONLY)) < 0)
		DO_EXIT_1("Failed to open file: %s\n", infile);

	indata = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (indata == NULL)
		DO_EXIT_1("Failed to open file: %s\n", infile);

	if (!memcmp(indata, "MSCF", 4))
		type = FILE_TYPE_CAB;
	else if (!memcmp(indata, "MZ", 2))
		type = FILE_TYPE_PE;
	else if (!memcmp(indata, msi_signature, sizeof(msi_signature)))
		type = FILE_TYPE_MSI;
	else
		DO_EXIT_1("Unrecognized file type: %s\n", infile);

	if (cmd != CMD_SIGN && type != FILE_TYPE_PE)
		DO_EXIT_1("Command is not supported for non-PE files: %s\n", infile);

	hash = BIO_new(BIO_f_md());
	BIO_set_md(hash, md);

	if (type == FILE_TYPE_CAB) {
		if (st.st_size < 44)
			DO_EXIT_1("Corrupt cab file - too short: %s\n", infile);
		if (indata[0x1e] != 0x00 || indata[0x1f] != 0x00)
			DO_EXIT_0("Cannot sign cab files with flag bits set!\n"); /* XXX */
	} else if (type == FILE_TYPE_PE) {
		if (st.st_size < 64)
			DO_EXIT_1("Corrupt DOS file - too short: %s\n", infile);
		peheader = GET_UINT32_LE(indata+60);
		if (st.st_size < peheader + 160)
			DO_EXIT_1("Corrupt PE file - too short: %s\n", infile);
		if (memcmp(indata+peheader, "PE\0\0", 4))
			DO_EXIT_1("Unrecognized DOS file type: %s\n", infile);
	} else if (type == FILE_TYPE_MSI) {
#ifdef WITH_GSF
		GsfInput *src;
		GsfInfile *ole;
		GSList *sorted = NULL;
		guint8 classid[16];
		gchar decoded[0x40];

		BIO_push(hash, BIO_new(BIO_s_null()));

		src = gsf_input_stdio_new(infile, NULL);
		if (!src)
			DO_EXIT_1("Error opening file %s", infile);

		sink = gsf_output_stdio_new(outfile, NULL);
		if (!sink)
			DO_EXIT_1("Error opening output file %s", outfile);

		ole = gsf_infile_msole_new(src, NULL);
		gsf_infile_msole_get_class_id(GSF_INFILE_MSOLE(ole), classid);

		outole = gsf_outfile_msole_new(sink);
		gsf_outfile_msole_set_class_id(GSF_OUTFILE_MSOLE(outole), classid);

		for (i = 0; i < gsf_infile_num_children(ole); i++) {
			GsfInput *child = gsf_infile_child_by_index(ole, i);
			const guint8 *name = (const guint8*)gsf_input_name(child);
			msi_decode(name, decoded);
			if (!g_strcmp0(decoded, "\05DigitalSignature"))
				continue;

			sorted = g_slist_insert_sorted(sorted, (gpointer)name, (GCompareFunc)msi_cmp);
		}

		for (; sorted; sorted = sorted->next) {
			GsfInput *child =  gsf_infile_child_by_name(ole, (gchar*)sorted->data);
			msi_decode(sorted->data, decoded);
			if (child == NULL)
				continue;

			GsfOutput *outchild = gsf_outfile_new_child(outole, (gchar*)sorted->data, FALSE);
			while (gsf_input_remaining(child) > 0) {
				gsf_off_t size = MIN(gsf_input_remaining(child), 4096);
				guint8 const *data = gsf_input_read(child, size, NULL);
				BIO_write(hash, data, size);
				if (!gsf_output_write(outchild, size, data))
					DO_EXIT_1("Error writing %s", outfile);
			}
			g_object_unref(child);
			gsf_output_close(outchild);
			g_object_unref(outchild);
		}

		BIO_write(hash, classid, sizeof(classid));
		g_slist_free(sorted);
#else
		DO_EXIT_1("libgsf is not available, msi support is disabled: %s\n", infile);
#endif
	}

	if (type == FILE_TYPE_CAB || type == FILE_TYPE_PE) {
		if (cmd != CMD_VERIFY) {
			/* Create outdata file */
			outdata = BIO_new_file(outfile, "w+b");
			if (outdata == NULL)
				DO_EXIT_1("Failed to create file: %s\n", outfile);
			BIO_push(hash, outdata);
		}
	}

	if (type == FILE_TYPE_CAB) {
		unsigned short nfolders;

		u_char cabsigned[] = {
			0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
			0xde, 0xad, 0xbe, 0xef, /* size of cab file */
			0xde, 0xad, 0xbe, 0xef, /* size of asn1 blob */
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
		};

		BIO_write(hash, indata, 4);
		BIO_write(outdata, indata+4, 4);

		tmp = GET_UINT32_LE(indata+8) + 24;
		PUT_UINT32_LE(tmp, buf);
		BIO_write(hash, buf, 4);

		BIO_write(hash, indata+12, 4);

		tmp = GET_UINT32_LE(indata+16) + 24;
		PUT_UINT32_LE(tmp, buf+4);
		BIO_write(hash, buf+4, 4);

		memcpy(buf+4, indata+20, 14);
		buf[4+10] = 0x04; /* RESERVE_PRESENT */

		BIO_write(hash, buf+4, 14);
		BIO_write(outdata, indata+34, 2);

		memcpy(cabsigned+8, buf, 4);
		BIO_write(outdata, cabsigned, 20);
		BIO_write(hash, cabsigned+20, 4); /* ??? or possibly the previous 4 bytes instead? */

		nfolders = indata[26] | (indata[27] << 8);
		for (i = 36; nfolders; nfolders--, i+=8) {
			tmp = GET_UINT32_LE(indata+i);
			tmp += 24;
			PUT_UINT32_LE(tmp, buf);
			BIO_write(hash, buf, 4);
			BIO_write(hash, indata+i+4, 4);
		}

		/* Write what's left */
		BIO_write(hash, indata+i, st.st_size-i);
	} else if (type == FILE_TYPE_PE) {
		unsigned int sigpos, siglen, nrvas;
		unsigned short magic;

		if (jp >= 0)
			fprintf(stderr, "Warning: -jp option is only valid "
					"for CAB files.\n");

		magic = GET_UINT16_LE(indata + peheader + 24);
		if (magic == 0x20b) {
			pe32plus = 1;
		} else if (magic == 0x10b) {
			pe32plus = 0;
		} else {
			DO_EXIT_2("Corrupt PE file - found unknown magic %x: %s\n", magic, infile);
		}

		nrvas = GET_UINT32_LE(indata + peheader + 116 + pe32plus*16);
		if (nrvas < 5)
			DO_EXIT_1("Can not handle PE files without certificate table resource: %s\n", infile);

		sigpos = GET_UINT32_LE(indata + peheader + 152 + pe32plus*16);
		siglen = GET_UINT32_LE(indata + peheader + 152 + pe32plus*16 + 4);

		/* Since fix for MS Bulletin MS12-024 we can really assume
		   that signature should be last part of file */
		if (sigpos > 0 && sigpos + siglen != st.st_size)
			DO_EXIT_1("Corrupt PE file - current signature not at end of file: %s\n", infile);

		if ((cmd == CMD_REMOVE || cmd == CMD_EXTRACT) && sigpos == 0)
			DO_EXIT_1("PE file does not have any signature: %s\n", infile);

		if (cmd == CMD_EXTRACT) {
			/* A lil' bit of ugliness. Reset stream, write signature and skip forward */
			BIO_reset(outdata);
			BIO_write(outdata, indata + sigpos, siglen);
			goto skip_signing;
		}

		if (cmd == CMD_VERIFY) {
			ret = verify_pe_file(indata, peheader, pe32plus, sigpos ? sigpos : fileend, siglen);
			goto skip_signing;
		}

		if (sigpos > 0) {
			/* Strip current signature */
			fileend = sigpos;
		}

		BIO_write(hash, indata, peheader + 88);
		i = peheader + 88;
		memset(buf, 0, 4);
		BIO_write(outdata, buf, 4); /* zero out checksum */
		i += 4;
		BIO_write(hash, indata + i, 60+pe32plus*16);
		i += 60+pe32plus*16;
		memset(buf, 0, 8);
		BIO_write(outdata, buf, 8); /* zero out sigtable offset + pos */
		i += 8;

		BIO_write(hash, indata + i, fileend - i);

		/* pad (with 0's) pe file to 8 byte boundary */
		len = 8 - fileend % 8;
		if (len > 0 && len != 8) {
			memset(buf, 0, len);
			BIO_write(hash, buf, len);
			fileend += len;
		}
	}

	if (cmd != CMD_SIGN)
		goto skip_signing;

	sig = PKCS7_new();
	PKCS7_set_type(sig, NID_pkcs7_signed);

	si = NULL;
	if (cert != NULL)
		si = PKCS7_add_signature(sig, cert, pkey, md);
	if (si == NULL) {
		for (i=0; i<sk_X509_num(certs); i++) {
			X509 *signcert = sk_X509_value(certs, i);
			/* X509_print_fp(stdout, signcert); */
			si = PKCS7_add_signature(sig, signcert, pkey, md);
			if (si != NULL) break;
		}
	}

	if (si == NULL)
		DO_EXIT_0("Signing failed(PKCS7_add_signature)\n");

	/* create some MS Authenticode OIDS we need later on */
	if (!OBJ_create(SPC_STATEMENT_TYPE_OBJID, NULL, NULL) ||
		!OBJ_create(SPC_MS_JAVA_SOMETHING, NULL, NULL) ||
		!OBJ_create(SPC_SP_OPUS_INFO_OBJID, NULL, NULL))
		DO_EXIT_0("Failed to add objects\n");

	PKCS7_add_signed_attribute
		(si, NID_pkcs9_contentType,
		 V_ASN1_OBJECT, OBJ_txt2obj(SPC_INDIRECT_DATA_OBJID, 1));

	if (type == FILE_TYPE_CAB && jp >= 0) {
		const u_char *attrs = NULL;
		static const u_char java_attrs_low[] = {
			0x30, 0x06, 0x03, 0x02, 0x00, 0x01, 0x30, 0x00
		};

		switch (jp) {
			case 0:
				attrs = java_attrs_low;
				len = sizeof(java_attrs_low);
				break;
			case 1:
				/* XXX */
			case 2:
				/* XXX */
			default:
				break;
		}

		if (attrs) {
			astr = ASN1_STRING_new();
			ASN1_STRING_set(astr, attrs, len);
			PKCS7_add_signed_attribute
				(si, OBJ_txt2nid(SPC_MS_JAVA_SOMETHING),
				 V_ASN1_SEQUENCE, astr);
		}
	}

	astr = ASN1_STRING_new();
	if (comm) {
		ASN1_STRING_set(astr, purpose_comm, sizeof(purpose_comm));
	} else {
		ASN1_STRING_set(astr, purpose_ind, sizeof(purpose_ind));
	}
	PKCS7_add_signed_attribute(si, OBJ_txt2nid(SPC_STATEMENT_TYPE_OBJID),
							   V_ASN1_SEQUENCE, astr);

	if (desc || url) {
		SpcSpOpusInfo *opus = createOpus(desc, url);
		if ((len = i2d_SpcSpOpusInfo(opus, NULL)) <= 0 ||
			(p = OPENSSL_malloc(len)) == NULL)
			DO_EXIT_0("Couldn't allocate memory for opus info\n");
		i2d_SpcSpOpusInfo(opus, &p);
		p -= len;
		astr = ASN1_STRING_new();
		ASN1_STRING_set(astr, p, len);

		PKCS7_add_signed_attribute(si, OBJ_txt2nid(SPC_SP_OPUS_INFO_OBJID),
								   V_ASN1_SEQUENCE, astr);
	}

	PKCS7_content_new(sig, NID_pkcs7_data);

#if 0
	for(i = 0; i < sk_X509_num(p7->d.sign->cert); i++)
		PKCS7_add_certificate(sig, sk_X509_value(p7->d.sign->cert, i));
#else
	if (cert != NULL)
		PKCS7_add_certificate(sig, cert);
	for(i = sk_X509_num(certs)-1; i>=0; i--)
		PKCS7_add_certificate(sig, sk_X509_value(certs, i));
#endif

	if ((sigdata = PKCS7_dataInit(sig, NULL)) == NULL)
		DO_EXIT_0("Signing failed(PKCS7_dataInit)\n");

	get_indirect_data_blob(&p, &len, md, type);
	len -= EVP_MD_size(md);
	memcpy(buf, p, len);
	unsigned char mdbuf[EVP_MAX_MD_SIZE];
	int mdlen = BIO_gets(hash, mdbuf, EVP_MAX_MD_SIZE);
	memcpy(buf+len, mdbuf, mdlen);
	int seqhdrlen = asn1_simple_hdr_len(buf, len);
	BIO_write(sigdata, buf+seqhdrlen, len-seqhdrlen+mdlen);

	if (!PKCS7_dataFinal(sig, sigdata))
		DO_EXIT_0("Signing failed(PKCS7_dataFinal)\n");

	/* replace the data part with the MS Authenticode
	   spcIndirectDataContext blob */
	astr = ASN1_STRING_new();
	ASN1_STRING_set(astr, buf, len+mdlen);
	dummy.type = V_ASN1_SEQUENCE;
	dummy.value.sequence = astr;
	sig->d.sign->contents->type = OBJ_txt2obj(SPC_INDIRECT_DATA_OBJID, 1);
	sig->d.sign->contents->d.other = &dummy;

#ifdef ENABLE_CURL
	/* add counter-signature/timestamp */
	if (turl && add_timestamp_authenticode(sig, turl, proxy))
		DO_EXIT_0("authenticode timestamping failed\n");
	if (tsurl && add_timestamp_rfc3161(sig, tsurl, proxy, md, mdbuf))
		DO_EXIT_0("RFC 3161 timestamping failed\n");
#endif

#if 0
	if (!PEM_write_PKCS7(stdout, sig))
		DO_EXIT_0("PKCS7 output failed\n");
#endif

	/* Append signature to outfile */
	if (((len = i2d_PKCS7(sig, NULL)) <= 0) ||
		(p = OPENSSL_malloc(len)) == NULL)
		DO_EXIT_1("i2d_PKCS - memory allocation failed: %d\n", len);
	i2d_PKCS7(sig, &p);
	p -= len;
	padlen = (8 - len%8) % 8;

	if (type == FILE_TYPE_PE) {
		PUT_UINT32_LE(len+8+padlen, buf);
		PUT_UINT16_LE(WIN_CERT_REVISION_2, buf + 4);
		PUT_UINT16_LE(WIN_CERT_TYPE_PKCS_SIGNED_DATA, buf + 6);
		BIO_write(outdata, buf, 8);
	}

	if (type == FILE_TYPE_PE || type == FILE_TYPE_CAB) {
		BIO_write(outdata, p, len);

		/* pad (with 0's) asn1 blob to 8 byte boundary */
		if (padlen > 0) {
			memset(p, 0, padlen);
			BIO_write(outdata, p, padlen);
		}
#ifdef WITH_GSF
	} else if (type == FILE_TYPE_MSI) {
		GsfOutput *child = gsf_outfile_new_child(outole, "\05DigitalSignature", FALSE);
		if (!gsf_output_write(child, len, p))
			DO_EXIT_1("Failed to write MSI signature to %s", infile);
		gsf_output_close(child);
		gsf_output_close(GSF_OUTPUT(outole));
		g_object_unref(sink);
#endif
	}

skip_signing:

	if (type == FILE_TYPE_PE) {
		if (cmd == CMD_SIGN) {
			/* Update signature position and size */
			(void)BIO_seek(outdata, peheader+152+pe32plus*16);
			PUT_UINT32_LE(fileend, buf); /* Previous file end = signature table start */
			BIO_write(outdata, buf, 4);
			PUT_UINT32_LE(len+8+padlen, buf);
			BIO_write(outdata, buf, 4);
		}
		if (cmd == CMD_SIGN || cmd == CMD_REMOVE)
			recalc_pe_checksum(outdata, peheader);
	} else if (type == FILE_TYPE_CAB) {
		(void)BIO_seek(outdata, 0x30);
		PUT_UINT32_LE(len+padlen, buf);
		BIO_write(outdata, buf, 4);
	}

	BIO_free_all(hash);
	hash = outdata = NULL;

	printf(ret ? "Failed\n" : "Succeeded\n");

	return ret;

err_cleanup:
	ERR_print_errors_fp(stderr);
	if (hash != NULL)
		BIO_free_all(hash);
	unlink(outfile);
	fprintf(stderr, "\nFailed\n");
	return -1;
}

/*
Local Variables:
   c-basic-offset: 4
   tab-width: 4
   indent-tabs-mode: t
End:
*/
