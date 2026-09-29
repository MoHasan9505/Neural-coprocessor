# MGPU Bridge NR check (experimental)

This checks whether your NVIDIA driver and your `nvngx_dlssnr.dll` can start DLSS-NR on each of your GPUs. No game runs during the check. It does not change your driver, the add-on or any game files.

## Get it

Use one of these:

- **Download the build.** Open the repository's **Actions** tab, then **nrcheck (experimental)**, then the latest successful run. Download `nrcheck-experimental` under **Artifacts**. You need to be signed in to GitHub.
- **Build it yourself.** Fork the repository, enable Actions in your fork's **Actions** tab, then run **nrcheck (experimental)** with **Run workflow**. The artifact appears on that run.

## How to run

1. Close the game.
2. Copy `nrcheck.exe`, `nvngx.dll_nrcheck.dll` and `Verify-NRCheck.ps1` into the game folder, beside the `mgpu\` folder.
3. Right-click `Verify-NRCheck.ps1` > **Run with PowerShell**.
4. Attach `nrcheck_results.zip` to your GitHub issue.

If PowerShell refuses to run the script, open a terminal in the game folder and run `nrcheck.exe`. It writes `nrcheck_report.txt` and the `nvngx_*_case*_adapter*.log` files. Attach those instead.

## What it tests

Each GPU is tested three times, each time in a fresh process:

- **Case 1:** the other GPU starts first, then this GPU. This is how the add-on runs today.
- **Case 2:** this GPU starts first, then the other GPU.
- **Case 3:** this GPU alone.

The check takes under a minute.
