# Recovery and rollback

Installation changes files for the next normal boot. The running module should
remain loaded. If any command reports failure, **keep the machine running** and
inspect its printed `/var/lib/sn8022-portable/…/transaction.json` and command logs.
There are verified copies of the prior module and initramfs in that directory.
An interrupted command may still be running; do not overlap it with another
DKMS/package operation or assume its completion from an incomplete log.

## Remove the only installed portable version

Run `sudo python3 manage.py remove` from that version's source directory. It
removes the DKMS registration, refreshes module lookup and the boot image, then
checks the selected module and actual image contents. Reboot only after success.
DKMS normally restores its archived original distribution module. That module
may lack speaker support, but provides the ordinary generic codec behavior.

## Return to a retained good portable release

1. Change to the retained good source, for example `/usr/src/sn8022-OLD`.
2. Run `sudo python3 manage.py install`. This directly reinstalls the good
   version and verifies its boot image; it does not first remove the active one.
3. After verification, remove the failed newer registration with
   `sudo dkms remove -m sn8022 -v NEW --all`, using the actual failed version.
   First inspect `dkms status`: if it has other kernel installations, handle
   those deliberately rather than removing them silently.
4. Run `sudo depmod -a "$(uname -r)"` and
   `sudo update-initramfs -u -k "$(uname -r)"`.
5. From the good source, run `sudo python3 manage.py verify`. Reboot after it
   passes. Leaving the newer version registered can make DKMS select it again
   during a later kernel update.

If a previous operation was interrupted while copying a module, DKMS may still
mark the old version installed even though its destination bytes are incomplete.
The verifier detects that mismatch. Do not use `--force` or reboot blindly;
inspect the transaction backups and the preserved DKMS build, and restore the
verified bytes with administrator help before rebuilding the boot image.

The release does not automate firmware power-state recovery. During development
a persistent speaker-silence state was recovered by booting the OEM Windows
driver. One later Linux sleep cycle passed, but indefinite suspend reliability
has not been established. Report this limitation rather than repeatedly sending
experimental codec commands.
