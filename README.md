# AI-RV64GC
An RV64GC emulator coded by **AI** (i don't revendicate anything over this, this is not my creation) with Internet support.
## In bulk trivia:
It boots the linux kernel (yay!), and can connect to the internet and do things such as `apk update`, or host an SSH server. The "distro" provided with the VM is Alpine Linux (latest version on the time of writing)\
On my machine (i3-10100f), it can do approximately 80 to 90 MIPS.
### Realised by `Claude Opus 4.6`, `Claude Sonnet 4.6`, `Gemini 3.1 Pro`, `Gemini 3 Flash`. 
## How to use:
### THIS IS MADE FOR LINUX **ONLY**
* You can either compile it yourself (`make`) and run it from here, or download files from the release tab.
* Once that's done, you can either build your own kernel (DON'T DO THAT FOR YOUR SANITY), or download the one provided in the downloads tab (under the name kernel.bin)
* Now you can download the disk (disk.img, in the release tab)
* and then you just have to run `./riscv-emu -d disk.img kernel.bin`
* if you want to have internet support (the host HAS to be linux, again), modify the `./network_host.sh` script (you can find it in the root of the repo) to replace `ens6p0` by the name of your network adapter, then run the script on your computer (**QUICK REMINDER: ALWAYS READ SCRIPTS BEFORE EXECUTING THEM**), and then run the command: `./riscv-emu -d disk.img -t tap0 kernel.bin`
* Once you're in the VM, run the script `setup.sh`, which is in the root folder.
* And you should be all set!
## Disclaimer
This not a project of mine: AI coded it so i don't want to take credits over that. If you want to credit this project, don't credit me, but the AIs. I post this here under my username only to show off what they did.
