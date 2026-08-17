// pam_auth.cpp — dlopen-backed PAM verification.
//
// The linux-pam headers are deliberately not included: the library is
// resolved at runtime, so the ABI types below are minimal mirrors of
// security/pam_appl.h and must stay layout-compatible with it.

#include "pam_auth.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>

namespace imgui_wasm_core {
namespace {

// linux-pam constants (security/pam_appl.h).
constexpr int kPamSuccess = 0;
constexpr int kPamConvErr = 19;
constexpr int kPamPromptEchoOff = 1;
constexpr int kPamErrorMsg = 3;
constexpr int kPamTextInfo = 4;

struct PamMessage {
    int msg_style;
    const char* msg;
};

struct PamResponse {
    char* resp;
    int resp_retcode;
};

struct PamConv {
    int (*conv)(int num_msg, const PamMessage** msg, PamResponse** resp, void* appdata_ptr);
    void* appdata_ptr;
};

struct ConvData {
    const char* password;
};

// Answers password prompts and swallows informational messages. Anything
// else (unexpected prompt styles, allocation failure) aborts the
// conversation rather than guessing.
int conv_fn(int num_msg, const PamMessage** msg, PamResponse** resp, void* appdata_ptr) {
    auto* data = static_cast<ConvData*>(appdata_ptr);
    auto* out = static_cast<PamResponse*>(calloc(size_t(num_msg), sizeof(PamResponse)));
    if (out == nullptr) return kPamConvErr;
    bool ok = true;
    for (int i = 0; i < num_msg && ok; i++) {
        switch (msg[i]->msg_style) {
            case kPamPromptEchoOff: {
                out[i].resp = strdup(data->password != nullptr ? data->password : "");
                if (out[i].resp == nullptr) ok = false;
                break;
            }
            case kPamErrorMsg:
            case kPamTextInfo:
                break;  // informational; no response expected
            default:
                ok = false;
                break;
        }
    }
    if (!ok) {
        // out was calloc'd: free() is safe on untouched (null) entries.
        for (int j = 0; j < num_msg; j++) free(out[j].resp);
        free(out);
        return kPamConvErr;
    }
    *resp = out;
    return kPamSuccess;
}

}  // namespace

struct PamAuth::Impl {
    void* lib = nullptr;
    int (*start)(const char*, const char*, const PamConv*, void**);
    int (*authenticate)(void*, int);
    int (*acct_mgmt)(void*, int);
    int (*end)(void*, int);
    const char* (*strerror)(void*, int);
};

PamAuth::PamAuth(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PamAuth::~PamAuth() {
    if (impl_ && impl_->lib) dlclose(impl_->lib);
}

std::unique_ptr<PamAuth> PamAuth::load(std::string* error) {
    void* lib = nullptr;
    for (const char* name : {"libpam.so.0", "libpam.so"}) {
        lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib != nullptr) break;
    }
    if (lib == nullptr) {
        if (error) *error = "libpam not found (tried libpam.so.0, libpam.so)";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->lib = lib;
    impl->start = reinterpret_cast<int (*)(const char*, const char*, const PamConv*, void**)>(
        dlsym(lib, "pam_start"));
    impl->authenticate =
        reinterpret_cast<int (*)(void*, int)>(dlsym(lib, "pam_authenticate"));
    impl->acct_mgmt = reinterpret_cast<int (*)(void*, int)>(dlsym(lib, "pam_acct_mgmt"));
    impl->end = reinterpret_cast<int (*)(void*, int)>(dlsym(lib, "pam_end"));
    impl->strerror =
        reinterpret_cast<const char* (*)(void*, int)>(dlsym(lib, "pam_strerror"));
    if (!impl->start || !impl->authenticate || !impl->acct_mgmt || !impl->end || !impl->strerror) {
        if (error) *error = "libpam is missing required symbols";
        return nullptr;
    }
    return std::unique_ptr<PamAuth>(new PamAuth(std::move(impl)));
}

bool PamAuth::verify(const std::string& service, const std::string& user,
                     const std::string& password, std::string* error) {
    if (!impl_) return false;
    ConvData data{password.c_str()};
    PamConv conv{conv_fn, &data};
    void* handle = nullptr;
    int rc = impl_->start(service.c_str(), user.c_str(), &conv, &handle);
    if (rc != kPamSuccess || handle == nullptr) {
        if (error) *error = std::string("pam_start: ") + impl_->strerror(nullptr, rc);
        return false;
    }

    // pam_strerror must run before pam_end frees the handle.
    rc = impl_->authenticate(handle, 0);
    if (rc != kPamSuccess) {
        if (error) *error = impl_->strerror(handle, rc);
        impl_->end(handle, rc);
        return false;
    }
    rc = impl_->acct_mgmt(handle, 0);
    if (rc != kPamSuccess) {
        if (error) *error = impl_->strerror(handle, rc);
        impl_->end(handle, rc);
        return false;
    }
    impl_->end(handle, kPamSuccess);
    return true;
}

}  // namespace imgui_wasm_core
