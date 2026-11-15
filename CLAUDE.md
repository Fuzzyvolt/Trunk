# CLAUDE.md

This repo has two uses:

1. **General questions from a phone.** Most sessions are plain Q&A; no files involved.
2. **The MicroZed embedded Linux project** (Buildroot, kernel drivers, and next the FPGA side), in `microzed/` and `buildroot-microzed-project.md`.

## How to respond

- Answer the question directly in chat. Lead with the answer, then add detail only if it helps.
- Keep replies short and easy to read on a phone screen: brief paragraphs, short lists, no wide tables.
- Use web search for anything current or factual that may have changed (news, prices, product details, schedules, versions). Cite sources with links.
- If a question is ambiguous, make a reasonable assumption, say what it was, and answer.

## MicroZed project

- Start by reading `buildroot-microzed-project.md` (board, Buildroot setup, repo workflow, lessons learned, current state). For a new project, also read its brief in `microzed/projects/` (index: `microzed/projects/README.md`).
- The user is learning: explain from the basics, with a small diagram first when it helps.
- The user runs commands on a Linux Mint laptop (`~/trunk` is the clone, `~/br-microzed` the Buildroot build, `~/mz-overlay` the rootfs overlay) and on the board (`ssh root@10.0.0.134`). Claude cannot reach either. Give commands in chat, one per code block, and say whether each runs on the laptop or the board.
- Code and files Claude writes go in `microzed/` (layout in `microzed/README.md`); the user gets them with `git pull` and uses `make`, `make push`, `make flash` in `microzed/`.
- Claude cannot build for the board here (no ARM toolchain or kernel tree): check syntax where possible, say what was not tested, and ask the user to paste build errors.
- When the user says "update the md", record what was done, what works, open problems and lessons learned in `buildroot-microzed-project.md`, and update the next steps.
- At the end of a project session, offer a pull request into the default branch so the next session sees the work.

## Files and git

- For general questions: do not create, edit, or commit files unless asked.
- If asked to save something (notes, research), put it in `notes/` and commit and push it, since the cloud session is wiped afterwards.
- For the MicroZed project: writing files in `microzed/` and updating `buildroot-microzed-project.md` is expected; commit and push them as you go.
- Never commit secrets or personal information (for example SSH keys: the board's host keys and the laptop's private key stay out of git).
