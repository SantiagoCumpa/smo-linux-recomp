# Third-party notices

Software of others that the repository carries. Everything else in it is original (GPL-3.0-or-later, see LICENSE).

| What | Where it comes from | License | In the repository |
|---|---|---|---|
| hactool, a static Linux x86-64 build | https://github.com/SciresM/hactool @ `1d64a83450e025622f3468c28fc4164dad2c5ef6` (the commit totk-pc-recomp uses), with `tools/hactool/bktr-new-layout.patch` (the 1.4.1 update's RomFS patch layout), built by `tools/build_hactool.py`; includes hactool's bundled mbedtls | ISC (`tools/bin/hactool.LICENSE`); mbedtls Apache-2.0 | `tools/bin/hactool` |

hactool decrypts the NSP files of your own game with your own keys (`tools/extract/nsp.py`); it contains no keys and
no game data. Its source is not in the repository: `tools/build_hactool.py --fetch` rebuilds the same binary from the
pinned commit and the patch.
