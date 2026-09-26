# Validation and listening checklist

## Existing driver evidence

The exact source checkpoint has been tested on one matching laptop with Ubuntu
7.0.0-34-generic and Secure Boot. Version 0.2.0 passed speaker playback, idle
suspend/resume, a normal reboot and one deep/S3 sleep cycle. Cold desktop starts
measured 605–612 ms including 250 ms of silence. Version 0.2.1 changes microphone
selection only; amplifier sources remain identical.

Version 0.2.1 passed normal reboot, the standard Capture Source control, actual
ADC selection of the internal pin, and user-accepted dictation with Bluetooth
off. The initial recording used high gain and had brief full-scale peaks;
quantitative microphone quality was not established. Adjust input volume through
normal sound settings if needed. Automatic external-headset mic detection is
disabled because the codec reports a mic even with an empty jack.

Focused extracted-kernel tests cover microphone selection/default/resume policy.
The wider parser harness passed 43 topology cases, 13 board-fixup cases and
274 assertions. These are offline tests with explicit stubs, not proof of all
physical behavior. Portable package results belong in `VALIDATION.md` when the
archive is built. The local development installer and portable installer are
distinct; successful use of one does not validate the other on another machine.

## Portable-package session regression

With the exact 0.2.1 source still installed, the user heard clean speaker tones
before and after a 12-second idle, clean left/right headphone output with the
speakers silent, and clean left/right speaker output after unplugging.
The codec and controller were suspended before both speaker starts. A 2.2-second
sample completed in about 2.57 seconds including stream startup/teardown.
The standard headphone jack and playback switches changed as expected, and
Capture Source remained Internal Mic through insertion/removal. This checks
routing persistence, not a new microphone recording. Runtime-resume messages
reported error=0 and guard_error=0. Familiar playback, volume up/down and
mute/unmute also passed the user check.

## Continuing validation before broader reliability claims

Use comfortable volume, no other audio playing, and the intended physical port.
Record the kernel, driver version and result, without publishing serial numbers,
MAC addresses, personal recordings or signing material.

1. Built-in speakers: familiar speech/music, left/right channel identification,
   volume and mute, and pause for at least ten seconds then resume.
2. Headphones without a microphone: insert, confirm output switches and speakers
   mute; remove, confirm output returns to speakers. Confirm the built-in mic
   remains selectable before and after this cycle.
3. Built-in mic: confirm Internal Mic and usable speech; repeat after idle.
4. An actual headset microphone, when available: manually select Mic, verify
   capture, then return to Internal Mic after removal. Ordinary headphones do
   not validate this feature.
5. Save work, suspend and resume, then check sound and mic. Expand to repeated
   cycles, cold boots and battery/AC transitions before stronger reliability
   claims. Stop and report any persistent silence instead of repeated probing.

Another matching physical laptop is needed before broad hardware-support claims.
The initial intended publication is an explicitly experimental source release.
