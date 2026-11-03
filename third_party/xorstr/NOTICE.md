# xorstr

Copyright 2017–2021 Justas Masiulis. Apache License 2.0.

The unmodified header was reused from this workspace's PlaySharp Loader protection library.
Upstream: <https://github.com/JustasMasiulis/xorstr>.

Project Chronicles disables AVX intrinsics for the x64 baseline. String obfuscation reduces plain-text
constants in the binary; it does not turn endpoints, protocol names or public signing keys
into secrets. Authentication credentials and signing private keys must never be embedded.
