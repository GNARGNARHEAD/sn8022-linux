# Secure Boot and the owner's DKMS key

The package relies on DKMS's normal signing setup. No private key or certificate
is distributed, and the installer never opens private key contents. DKMS may
create a local key if the administrator has not configured one.

With Secure Boot enabled, the pre-install check verifies the module's CMS
signature against public certificates exported from the enrolled MOK list.
Pending enrollment is not accepted as enrollment. This check is not a substitute
for verifying that the kernel actually loaded the expected module after reboot.

If installation stops because the signing certificate is not enrolled:

1. Read the **Public certificate (MOK)** path reported by your DKMS build.
2. Verify that it is your expected public certificate. Never import or share a
   private key. Inspect the administrator's DKMS configuration if the path is
   unexpected.
3. In your own terminal, run `sudo mokutil --import /path/to/public-certificate.der`.
   Some DKMS certificates use a `.pub` extension; use the actual public path.
4. Set a temporary enrollment password locally, reboot, and complete the MOK
   enrollment screen. Do not send the password to another person or service.
5. Verify enrollment with `mokutil --test-key /path/to/public-certificate.der`,
   then rerun `sudo python3 manage.py install` from the same source package.

Do not disable Secure Boot to work around a failed check. If the signing policy
or trust source differs from the tested MOK setup, stop and adapt the package
with evidence rather than bypassing the check.
