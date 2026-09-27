---
name: autonomous-loop
description: Drive the port continuously when the next step is not explicit, after a milestone, or when resuming.
---
# Autonomous loop

Read CLAUDE.md and the state docs, then repeat: pick the highest-value unresolved question
or bottleneck for the current phase (docs/NATIVE_PORT_PLAYBOOK.md) -> check
docs/LESSONS_LEARNED.md for a known answer -> falsifiable hypothesis -> smallest change or
instrumentation -> build -> run the scenario -> measure/compare (A/B, benchmarks) -> keep or
revert -> update docs -> commit -> next. Do not stop because an experiment failed; do not ask
the user to choose among engineering options. Brief Spanish status notes to the user.
