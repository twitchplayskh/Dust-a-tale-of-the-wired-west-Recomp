# Research provenance and licensing

## dreamREfactory

Source: https://github.com/dhobi/dreamrefactory
Pinned commit: `93d7e949e9e1e67269556dc48c316ead934a45ed`.
Licence: GNU GPL version 3, included in LICENSE.

The C++ resource, sprite, puppet, script-token and audio implementations are
adaptations informed by the GPL source, particularly engine/src/df/container.ts,
shp.ts, pup.ts, script.ts, audio.ts, image.ts and set-v1.ts. The typed VM also
uses runtime/parser.ts, interp.ts and their documented type
semantics as GPL references. Arithmetic, typing and control-flow behavior were
additionally corroborated against this installation's exact DF.EXE, as were
native SET table layouts and timing.
The dialogue host also uses runtime/builtins/puppets.ts and runtime/puppet.ts
as research references. Choice argument types, capacity and cancellation were
corroborated against this installation's DF.EXE; native worker synchronization
is a host implementation choice.
The project is therefore
provided under GPL-3.0-only. This is a language port and reconstruction, not a
claim to clean-room independence. Build/runtime do not depend on TypeScript.
The validation oracle imports the unmodified checkout directly.

Checkers (`src/checkers.cpp`) ports the pinned GPL `runtime/checkers.ts` search,
capture order, promotion and evaluation. `tests/checkers_golden.tsv` derives
from `engine/tests/golden/checkers-dll.json`: 611 defined DLL-emulator answers
and six undefined-buffer cases, for which the native host selects a deterministic
legal fallback. The reference corpus is upstream emulation evidence. Optional
MOV effects use pinned `mov-v1-effects.ts`; `tools/movie_effect_oracle.ts` creates
195 synthetic pixel hashes without game artwork. Matching local executable
decompilation corroborates transition dispatch and movie call/return layouts.

New context, movie, sound-bank, geometry and scaler work also adapts the pinned
engine/src/df/stg.ts,cst.ts,shp.ts,mov-v1.ts,trk.ts and runtime/geometry.ts,
sprite-scaler.ts,actors.ts,props.ts,scheduler.ts. The scaler's600 seeded cases
and recorded hashes are supplied by engine/tests/helpers/sprite-scaler-cases.ts
and engine/tests/golden/sprite-scaler.json. These exercise upstream engine
routines under its emulator; the upstream executable is not assumed to be
this installation's ALT31 engine. These attributions do not license game data.

The reference source credits DFET / M3tox for several format readers and codecs.
Those provenance comments are retained in the checkout; no claim of original
discovery is made for their layouts. Source names and GPL notices must accompany
distribution. No game data is licensed by the engine's source licence.

## DreamCatcher

Source: https://github.com/JamesK89/DreamCatcher
Pinned commit: `43c7f221c8aaf64eb96de842819e2636ad3cb927`.
No licence file or grant was found in this checkout. Its implementation is NOT
copied into this project. README and function address observations serve only as
research leads. Its DF.EXE SHA-1 is `97462977fc15277ba186a64baffe978d658413a9`,
which differs from this installation. Its injected-DLL design is not used.

## Tools and assets

REA 6.7.0 is acquired in tools/npm-cache; Ghidra 12.1.4, JDK 25, MSVC and SDK
remain existing external developer tools with their own licences. Generated
decompiler text is evidence, not recovered original source. Cloned research,
tool dependencies, private extracted previews, WAV files and build output are
excluded by .gitignore. The user supplies original game assets at runtime.

Optional local scaler verification imports the pinned GPL native-sprite Python
harness with a separately recovered ALT31 address map. Unicorn2.1.4 and
pefile2024.8.26 are installed only under tools/emulator-packages, excluded from
source delivery. They are developer tools with their own licences and are not
linked into the native game. The harness runs synthetic memory under an x86
emulator; it does not launch an original process or invoke host Windows APIs.
