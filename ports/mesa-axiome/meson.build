# meson.build for src/gallium/winsys/axiome/
# To be applied as a patch to ports/mesa/ via ports/mesa-axiome/patches/

axiome_winsys_sources = [
    'axiome_winsys.c',
    'axiome_softpipe.c',
]

axiome_winsys_lib = static_library(
    'axiome_winsys',
    axiome_winsys_sources,
    include_directories: [
        mesa_headers,
        gallium_headers,
        meson.current_source_dir(),
    ],
    dependencies: [
        util_dep,
        mesa_dep,
    ],
)
