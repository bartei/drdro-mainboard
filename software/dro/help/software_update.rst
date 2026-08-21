Update
======

Keeps the DRO up to date. One button updates everything.

The software running on this screen and the firmware running on the
board are released together and always carry the **same version
number**. Updating brings both to the same release, so the two halves
can never drift apart.

Fields
------

Installed Version
^^^^^^^^^^^^^^^^^
The version currently running. Read-only.

Available Version
^^^^^^^^^^^^^^^^^
The newest release published online. Checked automatically whenever
you open this page; tap it to check again. Requires an internet
connection.

Update
^^^^^^
Downloads and installs the release shown above — software **and**
firmware — then restarts. The button is disabled when you are already
up to date.

What happens when you press Update
----------------------------------

1. Both the software and the firmware are downloaded and checked for
   corruption.
2. The firmware image is saved to the DRO's own storage.
3. The new software is installed.
4. The DRO restarts.
5. On the way back up, the board is updated from the saved firmware
   image. This step needs no internet connection.

Nothing is installed until **both** downloads have finished and passed
their integrity check, so losing the network partway through leaves the
DRO exactly as it was.

Version mismatch warning
------------------------

If the board's firmware version does not match the installed software
version, an orange banner appears on the home screen. Tap it to come
here and update. The warning appears whether the board is older *or*
newer than the software — both mean the two halves are out of step.

This can happen if a board is swapped in from another machine, or if a
previous update was interrupted. Running the update resolves it.

Notes
-----

- An internet connection is needed to download an update, but not to
  finish applying one after the restart.
- If the board update does not complete, it is retried automatically
  the next time the DRO starts. The board keeps a working copy of its
  previous firmware and stays usable in the meantime.
- **Advanced…** opens firmware bank selection, manual flashing and the
  activity log. Those are diagnostic and recovery tools — the normal
  update path never needs them.
