# Extraction validation

Validated locally on 2026-10-29:

* Applied the extracted overlay to the pinned pristine workerd commit with zero
  fuzz. Compared the patched upstream files against the existing cached build's
  source: identical apart from the renamed target and an explanatory comment.
* Compiled the new host, both generated schemas, and bundle-admission test using
  the pinned workerd headers. Linked against the matching cached workerd objects.
* Built the independent Cap'n Proto 1.x control client with CMake.
* Ran the bundle-admission test successfully: named HTTP/native capability
  bindings, opaque IDs, duplicate names, empty capability names, missing main
  module, malformed JSON, and unsupported bundle versions.
* Ran `--version` successfully.
* Built a packaging smoke-test archive and verified its archive/binary SHA-256,
  schema and manifest contents, relative paths, executable permissions, and
  collected dependency licenses.
* Checked Python, shell, and workflow YAML syntax.

The local session could not complete a clean Bazel build: outbound dependency
fetches are blocked. Its sandbox also rejects `setsockopt(SO_REUSEADDR)`, preventing
workerd's internal listener from starting in either socket-path or inherited-FD
mode. The HTTP/native-RPC/lifecycle/resource-limit integration tests are provided
but have **not passed in this environment**. GitHub Actions runs both modes before
publishing; a successful clean CI build and runtime test run are still required.

Local smoke binaries/archives under `build/` use the machine's glibc 2.44 and are
not published release artifacts. The Ubuntu 24.04 workflow builds the distribution
binary and records its actual minimum glibc version in the release manifest.
No GitHub repository or release was created by this extraction.
