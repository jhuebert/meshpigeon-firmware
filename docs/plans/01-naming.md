# 01 — Naming

Requirement: a name that matches what the project does, is not already in use,
is a little clever, and **contains "MC", "Mesh", or "Hop"**.

## Recommendation: **MeshHop**

> *"Your message hops across the mesh."*

- **What it says.** Messages **hop** node-to-node across the **mesh** — literally
  how flood routing works. It hits *two* of the three acceptable roots at once.
- **Tone.** Short, bouncy, pronounceable, easy to say on a net ("send it over
  MeshHop"), and it names the product's actual mechanic without jargon.
- **Availability (checked 2026-09-19):**
  - GitHub: zero repositories matching `meshhop`.
  - npm registry: zero packages; PyPI: not found.
  - No known Play Store app of that name (final Play/F-Droid search still to be
    done by a human — see checklist below).
- **Suggested handles:** `github.com/meshhop-app` + `github.com/meshhop-firmware`
  (or one `meshhop` org with two repos). Android package: `app.meshhop.android`.

### Naming checklist before public launch (human tasks)

- [ ] Play Store + App Store search: "meshhop" / "mesh hop"
- [ ] F-Droid / fdroid.org search
- [ ] Domain check: `meshhop.app` (primary), `meshhop.net` (fallback)
- [ ] Quick USPTO / EUIPO trademark search for "MeshHop" in software class 9/42
- [ ] Google "meshhop app" / "meshhop radio" for collisions

## Candidates considered and rejected

Constraint satisfied (contains MC / Mesh / Hop) but rejected or demoted:

| Name | Why rejected / demoted |
|---|---|
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
| **MeshHop** (previous draft) | User rejected — lacks the MC/Mesh/Hop root |
| **Chainmail** (previous draft) | `kalix-systems/chainmail` already names an e2e group chat protocol |
| **Grapevine, Weft, Heddle, Emesh** (previous drafts) | Prior collisions and/or no MC/Mesh/Hop root |

Fallbacks in order if a human check finds a collision: **HopNet** → **MeshHopper**
→ revisit.

## Brand voice

- Friendly, plain-spoken, a little wry. Tagline options:
  - "Message anywhere. No towers, no internet."
  - "Hop, skip, and a message." / "One hop at a time."
- In-app language rules (see 07-ux): never say advert/flood/payload — say
  "share my contact", "message reached the mesh", "channel".

## Brand voice
