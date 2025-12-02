<!--
SPDX-License-Identifier: MIT
Copyright (c) 2025 WinUDPShardedEcho contributors
-->
This repository uses GitHub Copilot and the GitHub Copilot coding agent guidance.

Guidance for contributors:
- Keep responses concise and focused on code changes.
- Do not commit generated large transformations without a human review.
- When asked to modify repository files, ensure tests still pass locally.
 - Prefer small, targeted fixes: keep each change focused and minimal to address a single concern.

Additional requirements for automated agents (Copilot/coding agents):
- All commits produced or suggested by automated agents must include a Signed-off-by line (`-s`/`Signed-off-by:`) in the commit message. Do not create commits without this sign-off.
- Before creating or committing any changes, the agent must build the project and run the test suite locally. If the build or tests fail, the agent should not commit the changes and must instead provide the failing output and suggested fixes.
- If automated fixes are applied (formatting, linting), the agent should re-run build and tests before committing.

If you are a maintainer, include these steps in PR descriptions when asking Copilot to modify files:
- Which files to change.
- A concise spec of why the change is needed.
- Any tests or formatting checks to run after changes.

When submitting PRs, include a short checklist in the PR body confirming:
- Commits are signed-off.
- Build completed locally (tool and command used).
- Tests executed and passed (list of failing tests if any).
