# Security Policy

## Supported versions

ImGuiWasm is pre-1.0 software. Security fixes are applied to the latest code on
the `development` branch; older revisions are not maintained separately.

## Reporting a vulnerability

Do not disclose vulnerabilities in public issues or discussions. Use
[GitHub's private vulnerability reporting](https://github.com/n3in2019/imgui_wasm/security/advisories/new)
and include:

- the affected commit or version
- operating system, browser, and toolchain
- minimal reproduction steps
- expected and observed impact
- any suggested mitigation

You should receive an acknowledgement within seven days. Details and timelines
will be coordinated privately until a fix is available.

## Deployment warning

ImGuiWasm does not provide authentication or authorization. Do not expose it
directly to an untrusted network; place it behind appropriate access controls
and transport security.
