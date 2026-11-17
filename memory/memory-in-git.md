---
name: memory-in-git
description: "Claude's project memory lives in the repo at memory/ (the ~/.claude/projects/.../memory dir is a junction to it) so it is versioned with git"
metadata:
  type: project
---

2026-11-16 the user asked to "add memory to the included files for git". The files were moved to
C:/Users/Lurkz/Desktop/iw4-radiant/memory and ~/.claude/projects/C--Users-Lurkz-Desktop-iw4-radiant/memory
was replaced by a directory junction pointing there.

**Why:** the user wants the memory tracked and pushed with the project.
**How to apply:** memory edits show up in git status; include them in commits when the user asks to
commit/push. If the junction is ever missing, recreate it rather than writing a second copy.
