#include "providers/RuntimeSignature.hpp"

#include <Windows.h>
#include <Softpub.h>
#include <WinCrypt.h>
#include <WinTrust.h>

#include <cstddef>
#include <cwchar>
#include <vector>

namespace mfgdlss::providers
{
namespace
{
class CryptQueryObjects final
{
public:
    ~CryptQueryObjects()
    {
        if (message != nullptr) {
            static_cast<void>(CryptMsgClose(message));
        }
        if (store != nullptr) {
            static_cast<void>(CertCloseStore(store, 0));
        }
    }

    HCERTSTORE store{};
    HCRYPTMSG message{};
};

[[nodiscard]] std::string win32_failure(
    const char* const operation,
    const unsigned long status)
{
    return std::string(operation) + " failed with status " +
           std::to_string(status);
}
}

bool verify_runtime_signature(
    const std::filesystem::path& path,
    const std::wstring_view expected_publisher,
    std::string& failure)
{
    failure.clear();
    auto mutable_path = path.wstring();

    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = mutable_path.c_str();

    WINTRUST_DATA trust_data{};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;

    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto trust_status = WinVerifyTrust(
        static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust_data);

    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    static_cast<void>(WinVerifyTrust(
        static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust_data));

    if (trust_status != ERROR_SUCCESS) {
        failure = win32_failure(
            "Authenticode verification",
            static_cast<unsigned long>(trust_status));
        return false;
    }
    if (expected_publisher.empty()) {
        return true;
    }

    DWORD encoding{};
    DWORD content_type{};
    DWORD format_type{};
    CryptQueryObjects objects;
    if (!CryptQueryObject(
            CERT_QUERY_OBJECT_FILE,
            mutable_path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY,
            0,
            &encoding,
            &content_type,
            &format_type,
            &objects.store,
            &objects.message,
            nullptr)) {
        failure = win32_failure("Signer query", GetLastError());
        return false;
    }

    DWORD signer_size{};
    if (!CryptMsgGetParam(
            objects.message,
            CMSG_SIGNER_INFO_PARAM,
            0,
            nullptr,
            &signer_size) ||
        signer_size == 0) {
        failure = win32_failure("Signer information query", GetLastError());
        return false;
    }

    std::vector<std::byte> signer_storage(signer_size);
    if (!CryptMsgGetParam(
            objects.message,
            CMSG_SIGNER_INFO_PARAM,
            0,
            signer_storage.data(),
            &signer_size)) {
        failure = win32_failure("Signer information read", GetLastError());
        return false;
    }
    const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(
        signer_storage.data());

    CERT_INFO certificate_identity{};
    certificate_identity.Issuer = signer->Issuer;
    certificate_identity.SerialNumber = signer->SerialNumber;
    const auto* certificate = CertFindCertificateInStore(
        objects.store,
        encoding,
        0,
        CERT_FIND_SUBJECT_CERT,
        &certificate_identity,
        nullptr);
    if (certificate == nullptr) {
        failure = win32_failure("Signing certificate lookup", GetLastError());
        return false;
    }

    constexpr DWORD kPublisherCapacity = 256;
    wchar_t publisher[kPublisherCapacity]{};
    const auto publisher_size = CertGetNameStringW(
        certificate,
        CERT_NAME_ATTR_TYPE,
        0,
        const_cast<char*>(szOID_ORGANIZATION_NAME),
        publisher,
        kPublisherCapacity);
    CertFreeCertificateContext(certificate);
    if (publisher_size <= 1 || publisher_size > kPublisherCapacity) {
        failure = "the signing certificate has no usable publisher organization";
        return false;
    }

    const std::wstring expected(expected_publisher);
    if (_wcsicmp(publisher, expected.c_str()) != 0) {
        failure = "the signing certificate publisher does not match the expected vendor";
        return false;
    }
    return true;
}
}
