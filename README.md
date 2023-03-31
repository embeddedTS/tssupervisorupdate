# tssupervisorupdate
This project is used to update the supervisory microcontroller on embeddedTS products. This supports the TS-7250-V3, TS-7970, and more in the future.

# Build instructions:
Install dependencies:

    apt-get update && apt-get install git build-essential meson -y

Download, build, and install on the unit:

    git clone https://github.com/embeddedTS/tssupervisorupdate.git
    cd tssupervisorupdate
    meson setup builddir
    cd builddir
    meson compile
    meson install

# Usage
## TS-7250-V3
Grab the latest update for your system:
https://docs.embeddedts.com/TS-7250-V3#Microcontroller_Changelog

After running the above commands to install the updater, run this to update the TS-7250-V3:

    wget https://files.embeddedts.com/ts-arm-sbc/ts-7250-v3-linux/supervisory-firmware/ts7250v3-supervisor-update-latest.bin
    # Check if update is needed:
    tssupervisorupdate --dry-run --update ts7250v3-supervisor-update-latest.bin
    # Install update
    tssupervisorupdate --update ts7250v3-supervisor-update-latest.bin
    # Reboot to reload into new code
    reboot

## TS-7970
Grab the latest update for your system:
https://files.embeddedts.com/ts-arm-sbc/ts-7970-linux/supervisory-firmware/

This update will automatically reboot the system, so should be done from a read only filesystem, the image replicator, or accept the risk that this may corrupt your filesystem.

    wget https://files.embeddedts.com/ts-arm-sbc/ts-7970-linux/supervisory-firmware/ts7970-micro-update-latest.bin
    # Check if update is needed:
    tssupervisorupdate --dry-run --update ts7250v3-supervisor-update-latest.bin
    # Install update, immediately reboots after to apply the update
    tssupervisorupdate --update ts7250v3-supervisor-update-latest.bin

