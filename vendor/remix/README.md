# RTX Remix API header

`remix/remix_c.h` is the RTX Remix C API header, copied unmodified from
[Remix Plus](https://github.com/RemixProjGroup/dxvk-remix) (`public/include/remix/remix_c.h` at
`9aab34bd`, API version 0.1000.0). It is MIT licensed; the licence is at the top of the file.

It is used only for its types, by `code/midtown/agidx9/dx9remix.cpp`, which talks to the Remix API
through the 32-bit bridge. That file explains why this copy and how it copes with the other bridge
layouts in circulation. To update it, copy the new header over this one and check the
`static_assert`s in `dx9remix.cpp`.
