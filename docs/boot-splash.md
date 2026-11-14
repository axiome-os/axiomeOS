# Boot splash and debug mode

axiomeOS shows the axiomeOS logo with a rounded progress bar directly below
it while the UEFI loader (`axboot`) and the kernel bring the system up. The
bar advances as loader stages and kernel boot work complete; it does not add
a fixed delay.

## Configuration

`config/axboot.cfg` is copied to `\axboot.cfg` on the FAT boot partition and
read by the bootloader on every boot:

```ini
# kernel logs on the framebuffer
debug=0

# logo + rounded progress bar
splash=1
```

Edit the file and rebuild the boot partition (or the whole disk image):

```bash
nix develop -c make disk.img
```

### Debug mode

`debug=1` mirrors kernel `printk()` output to the framebuffer. Logs also keep
going to the serial port and `/var/log/kernel.log`.

`debug=0` (the default) keeps the framebuffer clean: the logo + progress bar
stay visible while booting and kernel logs are serial/file only. Kernel panics
always force framebuffer output regardless of the setting.

### Disabling the splash

`splash=0` (or `nosplash` on the kernel command line) disables the graphical
splash in both the bootloader and the kernel.

## Assets

The checked-in `include/bootsplash_assets.h` is generated from
`resources/axiome.svg` (the filled logo). Regenerate it after changing the
logo:

```bash
nix develop -c make assets
```

The generator (`tools/mkbootassets.py`) has no external image dependencies: it
rasterises the SVG path directly. The renderer in `include/bootsplash.h` is
shared by the bootloader and the kernel.
