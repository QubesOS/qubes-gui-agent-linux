# Qubes GUI Agent (Linux)

This is the agent side of the Qubes GUI protocol for Linux virtual machines,
part of [Qubes OS](https://www.qubes-os.org). It runs inside a VM and sends that
VM's windows to the GUI daemon in dom0 (or a dedicated GuiVM), so applications
from an isolated VM can be shown and used on the regular desktop without giving
the VM direct access to the display. The daemon side lives in
[qubes-gui-daemon](https://github.com/QubesOS/qubes-gui-daemon).

The repo also ships the matching audio support (PulseAudio and PipeWire modules)
and the VM-side X server glue that the agent builds on.

For the protocol and how the agent and daemon fit together, see the
[Qubes GUI documentation](https://www.qubes-os.org/doc/gui/).
