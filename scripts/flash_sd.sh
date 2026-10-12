#!/usr/bin/env bash
#
# Flash the wrapped preloader into the DE1-SoC microSD's raw 0xA2 partition.
#
# The card is ALREADY partitioned with the Cyclone V layout (0xA2 + FAT + Linux),
# so this deliberately does NOT reformat -- it writes only the 0xA2 slice that the
# boot ROM loads. Re-partitioning (and re-creating the exotic 0xA2 MBR type on
# macOS) is the dangerous part, and it is already done on this card.
#
# The SD is located DYNAMICALLY -- external + removable + carries an 0xA2
# partition -- because the /dev/diskN number changes across replugs. The internal
# system disk can never satisfy those three, and /dev/disk0 is refused outright.
#
# Run automatically by `make boot`. FLASH_DRYRUN=1 detects + prints, writes nothing.
#
set -eu

IMG="${1:-build/preloader.img}"
APP="$(dirname "$IMG")/app.bin"       # SDRAM app/test blob, read by load_from_sd()
APP_OFFSET_SECTORS=512                 # must match APP_OFFSET_SECTORS in bsp/sd.c (256 KiB in)

echo "flash_sd: locating the DE1-SoC microSD ..."

candidates=""
ncand=0
for d in $(diskutil list | grep -oE '^/dev/disk[0-9]+'); do
    info=$(diskutil info "$d" 2>/dev/null) || continue

    loc=$(echo "$info"  | awk -F': *' '/^ *Device Location/ {print $2; exit}')
    rem=$(echo "$info"  | awk -F': *' '/^ *Removable Media/  {print $2; exit}')
    virt=$(echo "$info" | awk -F': *' '/^ *Virtual/          {print $2; exit}')

    [ "$loc" = "External" ]   || continue    # never the internal SSD
    [ "$rem" = "Removable" ]  || continue
    [ "$virt" = "Yes" ] && continue          # never a synthesized APFS store

    # The distinctive Cyclone V marker: a partition of MBR type 0xA2.
    diskutil list "$d" | grep -q "0xA2" || continue

    candidates="$candidates $d"
    ncand=$((ncand + 1))
done

if [ "$ncand" -eq 0 ]; then
    echo "flash_sd: no external/removable disk with an 0xA2 partition found." >&2
    echo "          Insert the DE1-SoC microSD (it must already carry the" >&2
    echo "          Cyclone V layout: 0xA2 + FAT + Linux)." >&2
    exit 1
fi
if [ "$ncand" -gt 1 ]; then
    echo "flash_sd: more than one candidate disk -- refusing to guess:$candidates" >&2
    exit 1
fi

disk=$(echo $candidates)   # single element; word-splitting trims spaces

case "$disk" in
    /dev/disk0) echo "flash_sd: refusing /dev/disk0 (internal system disk)" >&2; exit 1 ;;
esac

a2=$(diskutil list "$disk" | awk '/0xA2/ {print $NF; exit}')
[ -n "$a2" ] || { echo "flash_sd: no 0xA2 slice on $disk" >&2; exit 1; }

echo "flash_sd: target $disk  (0xA2 partition /dev/$a2)"
diskutil info "$disk" | grep -E "Device / Media Name|Disk Size|Device Location|Removable Media" | sed 's/^/           /'

[ -f "$IMG" ] || { echo "flash_sd: image '$IMG' not found (run: make boot)" >&2; exit 1; }
img_bytes=$(wc -c < "$IMG" | tr -d ' ')

# Raw partition writes must be block-aligned, so conv=sync pads the last block.
# BS is the block size; the actual write rounds img up to a whole BS.
BS=65536
write_bytes=$(( ( (img_bytes + BS - 1) / BS ) * BS ))

# Guard: the padded write must fit inside the 0xA2 partition (never spill into
# the neighbouring FAT/Linux partitions).
a2_bytes=$(diskutil info "/dev/$a2" | grep -Eo '\([0-9]+ Bytes\)' | head -1 | grep -Eo '[0-9]+')
if [ -n "$a2_bytes" ] && [ "$write_bytes" -gt "$a2_bytes" ]; then
    echo "flash_sd: padded write ($write_bytes B) exceeds the 0xA2 partition ($a2_bytes B) -- refusing" >&2
    exit 1
fi

echo "flash_sd: image $IMG ($img_bytes B -> $write_bytes B written, 0xA2 slice is ${a2_bytes:-?} B)"

if [ "${FLASH_DRYRUN:-0}" = "1" ]; then
    echo "flash_sd: DRY RUN -- would: diskutil unmountDisk $disk && sudo dd if=$IMG of=/dev/r$a2 bs=$BS conv=sync"
    if [ -f "$APP" ]; then
        echo "flash_sd: DRY RUN -- would: sudo dd if=$APP of=/dev/r$a2 bs=512 seek=$APP_OFFSET_SECTORS conv=sync  ($(wc -c < "$APP" | tr -d ' ') B)"
    fi
    exit 0
fi

diskutil unmountDisk "$disk" >/dev/null 2>&1 || true

# Zero the WHOLE 0xA2 partition first: it is larger than our image, and any stale
# preloader (or extra mkpimage copies) left deeper in it can be picked up by the
# boot ROM instead of ours. After this, only our image can boot from this card.
if [ -n "$a2_bytes" ]; then
    echo "flash_sd: zeroing the 0xA2 partition ($a2_bytes B) to clear stale preloaders (sudo) ..."
    sudo dd if=/dev/zero of="/dev/r$a2" bs=$BS count=$(( a2_bytes / BS )) 2>/dev/null || true
fi

echo "flash_sd: writing image to /dev/r$a2 ..."
sudo dd if="$IMG" of="/dev/r$a2" bs=$BS conv=sync
sync

# Read the image region back and compare -- a raw write can report success but
# land nothing (as the first alignment failure did). Fail loudly if it mismatches.
echo "flash_sd: verifying ..."
if sudo dd if="/dev/r$a2" bs=$BS count=$(( write_bytes / BS )) 2>/dev/null \
        | head -c "$img_bytes" | cmp -s - "$IMG"; then
    echo "flash_sd: verify OK ($img_bytes bytes match)."
else
    echo "flash_sd: VERIFY FAILED -- the 0xA2 partition does not match the image." >&2
    exit 1
fi

# Place the SDRAM app blob inside the SAME 0xA2 partition, APP_OFFSET_SECTORS in --
# past the preloader (and the boot ROM's copy slots). load_from_sd() finds this
# partition's start LBA from the MBR and reads from here.
if [ -f "$APP" ]; then
    app_bytes=$(wc -c < "$APP" | tr -d ' ')
    app_end=$(( APP_OFFSET_SECTORS * 512 + app_bytes ))
    if [ -n "$a2_bytes" ] && [ "$app_end" -gt "$a2_bytes" ]; then
        echo "flash_sd: app.bin end ($app_end B) exceeds the 0xA2 partition ($a2_bytes B) -- refusing" >&2
        exit 1
    fi
    echo "flash_sd: writing app.bin ($app_bytes B) at sector $APP_OFFSET_SECTORS of the 0xA2 partition ..."
    sudo dd if="$APP" of="/dev/r$a2" bs=512 seek=$APP_OFFSET_SECTORS conv=sync
    sync
    rb=$(sudo dd if="/dev/r$a2" bs=512 skip=$APP_OFFSET_SECTORS count=$(( (app_bytes + 511) / 512 )) 2>/dev/null | head -c "$app_bytes" | cmp -s - "$APP" && echo OK || echo BAD)
    if [ "$rb" = OK ]; then
        echo "flash_sd: app.bin verify OK ($app_bytes bytes match)."
    else
        echo "flash_sd: app.bin VERIFY FAILED." >&2
        exit 1
    fi
fi

echo "flash_sd: done. Power-cycle the board to self-boot; then: python test.py --bootable"
