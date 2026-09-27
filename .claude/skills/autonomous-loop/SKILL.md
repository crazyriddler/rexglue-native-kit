---
name: autonomous-loop
description: Drive the port continuously when the next step is not explicit, after a milestone, or when resuming.
---
# Autonomous loop

1. docs/DECISION_GUIDE.md §1 (where am I) -> the current phase in docs/NATIVE_PORT_PLAYBOOK.md.
2. Run the loop of DECISION_GUIDE §2 on the highest-value item by the priorities of §3;
   use the symptom router (§4) before investigating anything already solved.
3. Record per DECISION_GUIDE §5, commit, repeat. A failed experiment is a result, not a stop.
4. Stop only for a real external blocker or at a milestone with PROJECT_STATE "Next actions"
   written; brief Spanish status note to the user.
