# 01 — Naming

Requirement: a name that matches what the project does, is not already in use,
is a little clever, and **contains "MC", "Mesh", or "Hop"**.

## Decision (2026-09-19): **MeshPigeon**

> *"Messages that find their way home."*

- **What it says.** A carrier pigeon is exactly the product: an off-grid
  message carrier that needs no towers, no internet, no infrastructure. It
  covers both halves of the project — the Android app (the pigeon) and the
  companion radio firmware (the bird's legs).
- **Mascot.** A pigeon mascot is available and usable as-is — it doubles as
  the app icon / adaptive icon and project logo.
- **Availability (checked 2026-09-19):**
  - GitHub: `meshpigeon`, `meshpigeon-firmware`, `meshpigeon-android` all 404 (free).
  - npm registry: no package; PyPI: not found.
  - RDAP: `meshpigeon.app` unregistered.
- **Handles:** `github.com/meshpigeon-firmware` + `github.com/meshpigeon-android`.
  Android package: `app.meshpigeon.android`.

### Naming checklist before public launch (human tasks)

- [ ] Play Store + App Store search: "meshpigeon" / "mesh pigeon"
- [ ] F-Droid / fdroid.org search
- [ ] Domain check: `meshpigeon.app` (primary), `meshpigeon.net` (fallback)
- [ ] Quick USPTO / EUIPO trademark search for "MeshPigeon" in software class 9/42
- [ ] Google "meshpigeon app" / "meshpigeon radio" for collisions

## History: previous recommendation was **MeshHop** (rejected by Jason)

The founding sessions used **MeshHop** ("your message hops across the mesh").
Availability checks at the time were clean, but the name was rejected in
review; the project was renamed to MeshPigeon on 2026-09-19 before any public
release (repos were still local-only, so the rename cost nothing).

## Candidates considered and rejected

Constraint satisfied (contains MC / Mesh / Hop) but rejected or demoted:

| Name | Why rejected / demoted |
|---|---|
| **MeshHop** | Rejected by Jason in review (2026-09-19) — replaced by MeshPigeon |
| **MeshCourier** | Active Kotlin decentralized Bluetooth-mesh messenger already uses it (`sunflowerthu/MeshCourier`, pushed 2026-05) — real collision, found on recheck |
| **MeshChat** | Already the de-facto name used in the MeshCore ecosystem (reticulum-meshchat, MeshCore companion) — guaranteed confusion |
| **MeshComm / Meshcom** | "Meshcom" has prior telecom use (historical Finnish Meshcom Technologies); too generic; search-results collision with MeshCore/Meshtastic |
| **Meshwork** | Prior apps exist (hackathon mesh contact-sharing, VR tool); too generic |
| **MeshC** | Awkward to say; "meshc" reads as a typo; no payoff |
| **MCChat** | Heavily occupied by Minecraft tooling (MCChatHUD, MCChatGPT, mcchat…) |
| **MC-hop / HopMC** | Forced; obscure what "MC" means to a newcomer |
| **Hopline** | Existing Discord/brewing community brand (`hopline/chatbot` et al.) |
| **Hopwire** | `thomaskiefer/hopwire` exists; also a WW2 anti-personnel mine — bad association |
| **Hopscotch** | Taken (kids coding platform) |
| **Hopper** | Existing decentralized mesh social app (`anon16767/hopper-android`); airline/generic collisions |
| **MeshGram / MeshRunner / MeshRelay** | GitHub repos already exist under these names |
| **Chainmail** | `kalix-systems/chainmail` already names an e2e group chat protocol |
| **Grapevine, Weft, Heddle, Emesh** | Prior collisions and/or no MC/Mesh/Hop root |

Runner-ups kept on the shortlist (all clean at check time): **MeshHerald**
(more formal — "the herald reports"), **MeshPorter**.

Fallbacks in order if a human check finds a collision: **MeshHerald** →
**MeshPorter** → **HopNet** → revisit.

## Brand voice

- Friendly, plain-spoken, a little wry. Tagline options:
  - "Message anywhere. No towers, no internet."
  - "Messages that find their way home."
  - "One hop at a time." (routing still hops — the pigeon just carries it)
- In-app language rules (see 07-ux): never say advert/flood/payload — say
  "share my contact", "message reached the mesh", "channel".
