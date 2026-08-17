// pam_auth.hpp — runtime-loaded PAM client for HTTP Basic verification.
//
// libpam is dlopen'ed at first use: the core keeps no link-time dependency
// and builds unchanged on systems without PAM. Each verify() runs a fresh
// pam_start / pam_authenticate / pam_acct_mgmt / pam_end conversation with
// its own handle, so concurrent calls from connection threads are safe.

#pragma once

#include <memory>
#include <string>

namespace imgui_wasm_core {

class PamAuth {
   public:
    // Loads libpam.so.0 (falling back to libpam.so). Returns nullptr and
    // fills `error` when the library or its required symbols are missing.
    static std::unique_ptr<PamAuth> load(std::string* error);

    ~PamAuth();
    PamAuth(const PamAuth&) = delete;
    PamAuth& operator=(const PamAuth&) = delete;

    // Verifies user/password against the named PAM service. Returns false
    // on any failure; `error` carries the pam_strerror text for logging
    // (wrong passwords included — callers decide what to print).
    bool verify(const std::string& service, const std::string& user,
                const std::string& password, std::string* error);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit PamAuth(std::unique_ptr<Impl> impl);
};

}  // namespace imgui_wasm_core
