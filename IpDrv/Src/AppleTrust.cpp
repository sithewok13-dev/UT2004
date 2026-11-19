// OpenSSL has no CA store on iOS and the system's can't be listed, so curl's OpenSSL checks each
// server's chain with the system's trust evaluation instead. Kept apart from the engine headers,
// whose check() and integer types the Apple headers would clash with.

#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Security/Security.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

static bool AddCertificate(CFMutableArrayRef Certs, X509* Cert)
{
	unsigned char* Der = NULL;
	int Len = i2d_X509(Cert, &Der);
	if (Len <= 0)
		return false;
	CFDataRef Data = CFDataCreate(NULL, Der, Len);
	OPENSSL_free(Der);
	SecCertificateRef Ref = Data ? SecCertificateCreateWithData(NULL, Data) : NULL;
	if (Data)
		CFRelease(Data);
	if (!Ref)
		return false;
	CFArrayAppendValue(Certs, Ref);
	CFRelease(Ref);
	return true;
}

static int VerifyWithSystem(X509_STORE_CTX* Ctx, void*)
{
	SSL* Ssl = (SSL*)X509_STORE_CTX_get_ex_data(Ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
	// curl sends the host name as SNI, an address has none and gets the chain checked without it.
	const char* Host = Ssl ? SSL_get_servername(Ssl, TLSEXT_NAMETYPE_host_name) : NULL;
	CFMutableArrayRef Certs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	// The leaf first, then the intermediates the server sent. Repeating the leaf does no harm.
	bool Ok = Certs && AddCertificate(Certs, X509_STORE_CTX_get0_cert(Ctx));
	STACK_OF(X509)* Chain = X509_STORE_CTX_get0_untrusted(Ctx);
	for (int i = 0; Ok && Chain && i < sk_X509_num(Chain); i++)
		AddCertificate(Certs, sk_X509_value(Chain, i));
	CFStringRef HostRef = Host ? CFStringCreateWithCString(NULL, Host, kCFStringEncodingUTF8) : NULL;
	SecPolicyRef Policy = SecPolicyCreateSSL(true, HostRef);
	SecTrustRef Trust = NULL;
	Ok = Ok && Policy && SecTrustCreateWithCertificates(Certs, Policy, &Trust) == errSecSuccess
		&& SecTrustEvaluateWithError(Trust, NULL);
	if (Trust)
		CFRelease(Trust);
	if (Policy)
		CFRelease(Policy);
	if (HostRef)
		CFRelease(HostRef);
	if (Certs)
		CFRelease(Certs);
	X509_STORE_CTX_set_error(Ctx, Ok ? X509_V_OK : X509_V_ERR_CERT_UNTRUSTED);
	return Ok ? 1 : 0;
}

// For CURLOPT_SSL_CTX_FUNCTION.
CURLcode AppleSslContext(CURL*, void* SslCtx, void*)
{
	SSL_CTX_set_cert_verify_callback((SSL_CTX*)SslCtx, VerifyWithSystem, NULL);
	return CURLE_OK;
}
