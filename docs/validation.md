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

A follow-up review ran the socket-path and inherited-FD integration tests outside
that session's socket-restricted sandbox. Both passed using the local smoke
binary. After the lifecycle and URL changes, the host translation unit was
recompiled and linked with the matching cached workerd objects, and the independent
1.x control client was rebuilt. Regression checks cover full binding URLs,
stop during a streamed response with a pending native receive, WebSocket echo
and stop, same-ID replacement, and suppression of delayed storage writes. A
standalone native-session test also verifies pending receive rejection and that
new sends/channel opens fail after revocation.

These are local smoke/integration results, not evidence of a clean release build
or Sandstorm feature/performance parity. A clean Bazel build, the packaged binary
inside Sandstorm's actual sandbox, and Sandstorm's feature, persistence, confinement, and comparative performance
acceptance checks remain required. The earlier
clean-build attempt was blocked by dependency downloads; it was not repeated as
part of this review.

Local smoke binaries/archives under `build/` use the machine's glibc 2.44 and are
not published release artifacts. The Ubuntu 24.04 workflow builds the distribution
binary and records its actual minimum glibc version in the release manifest.
No GitHub repository or release was created by this extraction.
