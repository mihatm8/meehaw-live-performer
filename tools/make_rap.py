#!/usr/bin/env python3
"""Generate the auto-rap vocal bank for the Vocal tracks from macOS text-to-speech (`say`).

Usage:
  python3 -I tools/make_rap.py [--out sdcard] [--genre House] [--keys A,E,B,...] [--dry-run] [--verify-only]
  then:  python3 -I tools/shard_folders.py sdcard/lp --max 64 && python3 -I tools/write_indexes.py sdcard --quiet

What it makes (docs/RAP_BANK.md has the long version):
  * a lyric generator: ~120 opener phrases, ~230 rhyme tails in 38 rhyme families, ~300 words with syllable
    counts; lines are one bar of eight 1/8-note slots at 125 BPM (240 ms); a word takes 1 slot (one syllable)
    or 2 slots (more), a line fills 6..8 slots and always ends inside its bar; couplets rhyme (AABB);
  * words rendered ONCE per voice and pitch with `say` (batched, split on the [[slnc]] pauses) into a cache,
    converted by ffmpeg to 22050 Hz mono 16-bit PCM, silence-trimmed, levelled, then fitted to their slot
    budget (ffmpeg atempo, never more than 2x) and fine-tuned to the key root (asetrate pitch shift of at most
    6 semitones after measuring the word's core pitch with a YIN estimator — male and female voices only,
    robots stay as they are);
  * per voice and key three product types assembled on the grid: ONE-SHOTS (1 bar, a shout or a 2..3 word
    phrase, token `1bars`), LOOPS (2 bars, a couplet or one line twice, `2bars`), SESSIONS (8 bars, four
    rhymed couplets, `8bars`). File name:  <Key>min__125bpm__RAP<M|F|R>__<n>bars__<slug>.wav  (slug <= 24 chars
    of [A-Za-z0-9_]); robot files also go to a small keyless NA/ set.
  * Key: male voices sit on the root in octave 2/3 (MIDI 41..52), female in 3/4 (53..64); the `say` pitch base
    ([[pbas N]]) is calibrated per voice against measured f0 (cache/calib), the fine-tune does the rest.
  * Loudness as tools/import_library.py's vocal path: file peak -1 dBFS; words are levelled to a common
    loudest-20 ms RMS first.  All output is verified without listening: duration = bars x 1.92 s, peak, every
    name parses (key, bpm token, bars token, slug), onsets sit on the 1/8 grid, word pitch vs key root.

Output folder: <out>/lp/packs/<genre>/vocal/<Key>min/ (or <Key>min/rap/ when the key folder is already a shard
root of sNN subfolders — an indexed folder must stay a leaf, SPEC §10.1). Existing RAP files of this tool are
removed first (--keep-existing disables). Stdlib + ffmpeg + `say` only; run with -I.

2026-10-16 (research docs/RESEARCH_LIBRARY_EXPANSION.md §3, phase V1): eight natural voices (M: Rocko, Reed, Evan
Enhanced, Eddy US; F: Karen, Moira, Tessa, Allison Enhanced), a lexicon of 366 one-shot texts / 327 heads /
583 tails in 66 rhyme families, and a planner that never repeats a line inside a key folder nor a slug inside
(folder, class, bars) (PLAN_STATS, printed as `plan: ...`). The 150-files-per-key bank used for the staging mirror:
  python3 -I tools/make_rap.py --out sdcard/lp_stage/vocals --oneshots-mf 30 --loops-mf 30 --sessions-mf 5 \
          --oneshots-r 12 --loops-r 6 --sessions-r 2 --na-oneshots 8 --na-loops 4 --na-sessions 1
(= 60 + 60 + 30 natural / robot files per key, 13 in NA; ≈ 25 MB per key). The defaults below are still the small
19-per-key bank of docs/RAP_BANK.md.
"""
import argparse
import concurrent.futures as cf
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import time
import wave
from array import array
from operator import mul

RATE = 22050
BPM = 125
SLOTS_PER_BAR = 8
SLOT = RATE * 60 * 4 // (BPM * SLOTS_PER_BAR)            # 5292 samples = 240 ms
BAR = SLOT * SLOTS_PER_BAR                                # 42336 samples = 1.92 s
SLOT_FILL = 0.92                                          # a word may use this much of its slot budget
WORD_RMS_DB = -10.0                                       # loudest-20 ms RMS target per word
PEAK_DB = -1.0                                            # file peak (import_library.py vocal path)
TRIM_DB = -54.0                                           # silence trim threshold (import_library.py)
TUNE_MAX_ST = 6.0                                         # largest fine-tune shift (resampling moves formants too)
KEYS = ["A", "E", "B", "F#", "C#", "G#", "D#", "A#", "F", "C", "G", "D"]   # the 12 natural-minor keys
PC = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}
NAME_RE = re.compile(r"^(?P<key>(?:[A-G]#?min)|NA)__(?P<bpm>\d{2,3})bpm__RAP(?P<v>[MFR])__(?P<bars>\d+)bars__(?P<slug>[A-Za-z0-9_]{1,24})\.wav$")
RAP_GLOB_RE = re.compile(r"__\d{2,3}bpm__RAP[MFR]__\d+bars__[A-Za-z0-9_]+\.wav$")

# ------------------------------------------------------------------------------------------------ voices

# class -> list of (say voice name, centre MIDI note of the comfortable range, f0 search range Hz).
# Chosen by measuring every installed English voice (docs/RAP_BANK.md): Karen and Moira hold their pitch
# within ~2 semitones over a word with no creak; the male voices all drop to a pitch floor in the last ~70 ms
# of a word above pbas ~45 — Rocko and Reed least. Daniel/Samantha (the obvious picks) creak the most and
# are not used. The pitch-modulation command [[pmod]] is ignored by all of them.
# 2026-10-16: four more natural voices (research docs/RESEARCH_LIBRARY_EXPANSION.md §3 option F), picked by the same
# measurement over every installed English voice at pbas 40 / 60 (eight words each): Tessa (voiced 1.00, tail creak
# 0.00, like Karen / Moira) and Allison Enhanced (0.82 / 0.25) for F; Evan Enhanced (0.43 / 0.21) and Eddy US
# (0.52 / 0.28) for M, both creaking less than Rocko / Reed (0.44 / 0.41). Rejected: Samantha, Flo, Shelley (creak
# > 0.7), Daniel (voiced 0.33), Sandy / Ava Premium (creak ≈ 0.5). Files alternate over the voices of a class.
VOICES = {
    "M": [("Rocko (English (US))", 46.5, (60, 300)), ("Reed (English (US))", 46.5, (60, 300)),
          ("Evan (Enhanced)", 46.5, (60, 300)), ("Eddy (English (US))", 46.5, (60, 300))],
    "F": [("Karen", 58.5, (110, 520)), ("Moira", 58.5, (110, 520)),
          ("Tessa (English (South Africa))", 58.5, (110, 520)), ("Allison (Enhanced)", 58.5, (110, 520))],
    "R": [("Zarvox", None, (60, 400)), ("Trinoids", None, (60, 500)), ("Fred", None, (60, 300))],
}
NOVELTY = ["Cellos", "Bells", "Bad News"]                # "robot" singing voices, NA one-shots only
CALIB_WORDS = ["body", "tonight", "everybody", "rhythm", "move", "dance", "floor", "night"]
CALIB_PBAS = list(range(30, 73, 3))

# ------------------------------------------------------------------------------------------------ lexicon

# word -> syllables. Slots: 1 syllable -> 1 slot, more -> 2 slots (16th-note syllables, rap flow).
SYL = {}
def _syl(n, words):
    for w in words.split():
        SYL[w] = n
_syl(1, "the your my to it up and in on all of a with let's don't we you i can got get like just this is are come so "
        "me us our no now yeah oh hey uh hands feel move pump jump dance down night long go stop rise floor lights "
        "bass drum beat sound fire wire bounce work shake make take break wake light bright right tight ground found "
        "chance trance drive dive heat street sweet feet flow show glow slow low know top drop play way day stay time "
        "climb prime groove smooth free be see sky high fly why strong song rain eyes skies here near clear shine "
        "mine line loud crowd proud stand real town crown run fun sun one done heart start back track how wow name "
        "flame game same kick quick burn turn soul roll whole fast last blast past place space face race zone own "
        "ride wave vibe mind lose set touch catch bring hear young noise more two three star mix air hold them that's "
        "clap put raise came rock watch win built have reach say give look lost spin from out far at its it's pure "
        "us ask bow close comes cross do fall flight let new once part sing sleep some steal trick wide wild")
_syl(2, "tonight higher body rhythm music party people gonna wanna never ever louder harder faster stronger deeper "
        "baby ignite alright around romance alive arrive survive repeat complete nonstop away okay today awake again "
        "insane surprise divine unreal begun apart restart attack return control motion ocean nation alone along "
        "goodbye crystal into disguise follow nothing freedom power answer upside number brand burning ready "
        "open total nobody outer belong")
_syl(3, "everybody desire forever together energy melody paradise disappear understand electric emotion devotion "
        "sensation vibration radio")
_syl(4, "celebration")
# 2026-10-16 growth (research §3 option F: one-shot texts 63 -> 200+, more heads and tails so a 150-file key folder
# repeats no line and no slug)
_syl(1, "hot cold gold bold hold sold told old wild child smile dream scream beam team steam cream green queen scene "
        "seen clean mean lean deep keep sweep leap rush crush hush much such luck stuck bump thump grind find blind "
        "kind most ghost coast toast host post fade shade made paid save cave crave brave gave praise days blaze gaze "
        "phase haze maze lift drift gift shift swift rip trip skip flip grip dip hip zip slip whip chip ship blow throw "
        "snow grow throne tone stone phone bone lane plane train brain chain gain main pain spray pray sway may lit hit "
        "sit bit fit grit split spit quit boom zoom room doom bloom tune moon soon noon june swing ring king thing wing "
        "spring sting fling cling bling drip tease please squeeze breeze freeze knees seas keys trees ease fresh hard "
        "yard card large spark dark park mark shark art smart chart glass class brass pass mass pulse ho yo whoa ooh ah "
        "la na da bam pow bang zap drums hum roar soar door core score shore storm warm form born horn torn sworn dawn "
        "dusk bomb calm choose cruise news blues shoes prove rule cool pool fool school tool slide glide hide pride side "
        "tide wide thrive wrong none quit off hear leave through than what will as roof wall left forth club stage "
        "max shoot stars ten nine eight goes stops crank won't did behind")
_syl(2, "sunrise sunset moonlight starlight midnight daylight spotlight sunlight heartbeat heartbreak sunshine rolling "
        "falling calling rising shining flying dancing jumping bouncing moving grooving pumping burning turning shaking "
        "breaking taking making waking living racing dropping going stopping having echo tempo disco techno vinyl neon "
        "laser mirror thunder lightning whisper crazy lazy hazy maybe lady faded wasted hollow borrow sorrow pressure "
        "pleasure treasure measure forward onward upward heaven seven clever wonder under over hover cover lover summer "
        "winter season reason kingdom wisdom system vision mission anthem rocket pocket sonic tonic magic basic static "
        "skyline lifeline frontline bassline headline online collide inside outside worldwide riding sliding gliding "
        "hiding parade ablaze behind quiet better colors after transform tender render mellow surrender candy weekend "
        "friday morning second city hours bodies bullet center bottom ceiling dancer fever volume limits moment ready "
        "dusk countdown")
_syl(3, "eternal infinite overload overdrive overflow afterglow undertow tomorrow yesterday hurricane tornado volcano "
        "wonderful beautiful dynamite satellite carnival festival magical musical gravity galaxy fantasy ecstasy destiny "
        "harmony symphony remedy memory victory history mystery liberty wherever however remember september november "
        "december defender saturday superstar underground")
_syl(4, "revolution evolution euphoria adrenaline unstoppable incredible invincible unbreakable")
_syl(1, "across arms been best big by can't closed crack eye fear fight fill first fold for front heads hearts hop house join "
        "lead life live loose love nights not or paint pick push round rules runs shut soft solve spread step tear tech till "
        "too wait want wear were")
_syl(1, "taste")
_syl(2, "every golden sugar revive")
SYL["surrender"] = 3
SYL["ablaze"] = 2
SYL["behind"] = 2
SYL["nobody"] = 3
SYL["everybody"] = 4
SYL["dj"] = 2

# rhyme families: name -> words (the LAST word of a tail decides its family)
FAMILIES = {
    "IGHT": "tonight light bright night flight right tight ignite alright midnight moonlight starlight spotlight daylight sunlight dynamite satellite",
    "IRE": "fire higher desire wire",
    "OUND": "sound ground around found",
    "ANCE": "dance chance trance romance",
    "IVE": "alive drive dive arrive survive thrive revive",
    "EAT": "beat heat street sweet feet repeat complete heartbeat",
    "OW": "go flow show glow slow low know blow throw snow grow",
    "OP": "stop top drop nonstop",
    "AY": "play way day stay away okay today spray pray sway may",
    "IME": "time climb prime",
    "OOVE": "move groove smooth",
    "EE": "free be me see we energy melody harmony symphony gravity galaxy fantasy destiny victory memory history mystery liberty ecstasy remedy",
    "Y": "sky high fly why goodbye",
    "ONG": "long strong song along belong wrong",
    "AIN": "again rain insane lane plane train brain chain gain main pain",
    "ISE": "rise eyes skies surprise paradise sunrise",
    "AKE": "shake take make break wake awake",
    "UMP": "jump pump bump thump",
    "ODY": "body nobody everybody",
    "EAR": "here near clear disappear",
    "INE": "shine mine line divine skyline lifeline bassline frontline headline online",
    "OUD": "loud crowd proud",
    "AND": "hands stand understand",
    "EEL": "feel real unreal",
    "OWN": "down town crown",
    "UN": "run fun sun one done begun none",
    "ART": "heart start apart restart part art smart chart",
    "ACK": "back track attack",
    "OWW": "now how wow bow",
    "AME": "name flame game same",
    "ICK": "kick quick electric trick",
    "URN": "burn turn return",
    "OL": "control soul roll whole",
    "AST": "fast last blast past",
    "ACE": "place space face race bass",
    "OTION": "motion emotion ocean devotion",
    "ATION": "sensation vibration celebration nation",
    "ONE": "zone own alone throne tone stone phone bone",
    "OLD": "gold bold hold cold sold told old fold",
    "EAM": "dream scream beam team steam cream",
    "EEN": "green queen scene seen clean mean",
    "EEP": "deep keep sweep leap sleep",
    "USH": "rush crush hush",
    "IND": "grind find blind kind mind behind",
    "OST": "most ghost coast toast host post",
    "ADE": "fade shade made paid parade",
    "AZE": "raise praise days blaze gaze phase haze maze ablaze",
    "IFT": "lift drift gift shift",
    "IP": "rip trip skip flip grip dip hip zip whip drip slip",
    "IT": "lit hit sit fit split spit quit bit",
    "OOM": "boom zoom room bloom",
    "OON": "tune moon soon june",
    "ING": "swing ring king thing wing sing spring sting bling bring",
    "EEZE": "tease please squeeze breeze freeze knees keys trees ease",
    "ARK": "spark dark park mark shark",
    "ASS": "glass class pass mass",
    "ORE": "more core score shore floor door roar soar",
    "ORM": "storm warm form transform",
    "ORN": "born horn torn sworn",
    "OOL": "cool pool rule school fool tool",
    "OOSE": "lose choose cruise news blues shoes",
    "IDE": "ride wide collide inside outside worldwide slide glide hide pride side tide",
    "EVER": "forever together whatever wherever never clever ever",
    "ENDER": "surrender defender tender render",
    "UTION": "revolution evolution",
    "OLLOW": "follow hollow",
}
FAMILY_OF = {w: f for f, ws in FAMILIES.items() for w in ws.split()}

# tails: phrases that END a line on a rhyme word (2..5 slots)
TAILS = """
tonight | all night | feel the light | shine so bright | take flight | into the night | hold on tight | we ignite | it's alright | turn on the light
feel the fire | take me higher | my desire | we go higher | set it on fire | burning fire | higher and higher
hear the sound | shake the ground | spin around | to the ground | feel the sound | turn around | lost and found | pump up the sound
let's dance | take a chance | in a trance | this romance | feel the trance | last chance | we dance | come and dance
come alive | feel alive | we survive | take a dive | we arrive | stay alive | feel the drive
to the beat | feel the heat | on the street | so sweet | move your feet | on repeat | bring the heat | feel the beat | make it complete
let it flow | here we go | steal the show | let's go | lights down low | go go go | ready set go | run the show
don't stop | to the top | bass drop | never stop | we don't stop | let it drop | nonstop
come and play | all the way | here to stay | night and day | fly away | let's play | feel okay | dance all day | it's today
one more time | it's our time | we climb | all the time | take your time | in time
feel the groove | make your move | let's move | so smooth | in the groove | move move move | make a move
set me free | you and me | dance with me | feel the energy | come with me | we are free | wild and free | feel the melody | let it be | follow me
touch the sky | fly so high | don't ask why | to the sky | so high | we fly | reach the sky | say goodbye
all night long | feel so strong | sing along | to the song | we belong | so strong | this is our song | come along
once again | feel the rain | go insane | dance in the rain | do it again | play it again
we rise | close your eyes | to the skies | paradise | open your eyes | watch us rise | no surprise
make it shake | wide awake | feel the break | shake shake shake | let it break | give and take
jump jump | everybody jump | pump pump pump | jump jump jump | make them jump
move your body | everybody | nobody | shake your body | work your body | rock your body | free your body
right here | crystal clear | disappear | we are here | loud and clear | far and near | come near
let it shine | you are mine | feel divine | cross the line | shine shine shine | so divine
so loud | in the crowd | feel so proud | turn it loud | play it loud | move the crowd
raise your hands | clap your hands | make a stand | take a stand | understand | wave your hands
feel real | the way i feel | so unreal | this is real | i can feel | make me feel
get down | shake the town | take the crown | burn it down | break it down | upside down
we run | have fun | feel the sun | we are one | just begun | here comes the sun | it's done | number one
my heart | let's start | fall apart | a brand new start | feel my heart | from the start | play your part
turn back | on the track | attack | never look back | bring it back | to the track
right now | show me how | wow | do it now | here and now | take a bow
say my name | feel the flame | play the game | never the same | feel the same | in the game | light the flame
feel the kick | make it quick | electric | do the trick | that's the kick
let it burn | watch it burn | take your turn | no return | feel the burn | we return
take control | feel my soul | let it roll | lose control | rock and roll | out of control | free your soul
so fast | built to last | have a blast | from the past | move so fast | make it last
feel the bass | in this place | drop the bass | to the bass | in your face | outer space | win the race | pump the bass
in motion | pure emotion | like the ocean | feel the motion | stay in motion | total devotion
sensation | vibration | celebration | feel the vibration | one nation | dance sensation
in the zone | on my own | never alone | all alone | stay in the zone | set the tone | take the throne | like a stone | pick up the phone
heart of gold | we go bold | take a hold | hot and cold | never grow old | brave and bold | we never fold
living the dream | hear me scream | ride the beam | we are the team | blow off steam | like a dream | make them scream
neon green | dance floor queen | steal the scene | never been seen | keep it clean | know what i mean | we are the queen
in too deep | we don't sleep | run so deep | the bass runs deep | love we keep | no time to sleep | in so deep
feel the rush | hear the crush | in a rush | don't you hush | sugar rush | what a rush | feel the crush
lose your mind | leave it behind | free your mind | one of a kind | feel the grind | love is blind | hard to find | blow your mind
feel the most | coast to coast | like a ghost | raise a toast | we want the most | dance like a ghost
never fade | in the shade | join the parade | we got it made | colors fade | lead the parade | never ever fade
set ablaze | hands we raise | in a haze | through the haze | better days | lost in the maze | feel the blaze | golden days
feel the lift | catch the drift | this is a gift | night shift | make the shift | we drift | let it lift
take the trip | let it rip | feel the drip | tight grip | never slip | crack the whip | flip flip flip | feel the trip
we are lit | one more hit | never quit | feel the hit | make it fit | do your bit | we don't quit | keep it lit
hear the boom | zoom zoom zoom | fill the room | shake the room | watch it bloom | boom boom boom | light up the room
to the moon | hum the tune | see you soon | under the moon | over the moon | sing the tune | dance to the tune
feel the swing | let it ring | we are the king | do your thing | hear it ring | bling bling | let it swing | make it ring
feel the breeze | do as you please | on your knees | time will freeze | ride the breeze | shake the trees | hold the keys | we don't freeze
light the spark | in the dark | after dark | leave your mark | out of the dark | feel the spark | hit the mark | glow in the dark
raise your glass | top of the class | let it pass | move the mass | first class | this will pass | we are the class
give me more | hit the floor | shake the floor | open the door | hear us roar | watch us soar | to the core | we want more | more more more
ride the storm | feel the storm | keep me warm | we transform | take form | in the storm | eye of the storm
we are born | hear the horn | night is torn | blow the horn | a star is born | we were born
stay cool | break the rule | in the pool | we rule | so cool | old school | play it cool | we make the rule
nothing to lose | you can choose | spread the news | on a cruise | we can't lose | sing the blues | dancing shoes | we never lose
feel the ride | side by side | worldwide | come inside | let's collide | take the ride | deep inside | step outside | let it slide | run and hide | feel the pride | turn the tide
ride the train | feel it in your brain | break the chain | in the fast lane | on a plane | all we gain | no more pain | feel no pain | hop on the train | lose the chain
watch it grow | let it blow | let it snow | we grow | blow by blow | watch us grow
after midnight | in the moonlight | under starlight | in the spotlight | dance in the daylight | feel the sunlight | like dynamite | like a satellite | past midnight
feel my heartbeat | hear the heartbeat | one heartbeat
let it spray | come what may | hear us pray | feel the sway | we sway | watch us sway
feel the bump | hear the thump | bump bump bump | feel the thump
live forever | dance forever | now or never | all together | we are clever | better than ever | you and me forever | live together | now and forever | the best ever
we surrender | so tender | night defender | we never surrender | i surrender
this is the revolution | start the revolution | we are the evolution | feel the evolution | dance revolution | sound revolution
we follow | feel so hollow | you follow | just follow | hearts so hollow | we never follow
feel the harmony | hear the symphony | fight the gravity | across the galaxy | in a fantasy | this is destiny | taste the victory | make a memory | this is history | solve the mystery | feel the liberty | pure ecstasy | be my remedy | we are the energy | sweet harmony | dance in harmony
a new sunrise | till the sunrise | wait for the sunrise | watch the sunrise | dance till sunrise
on the skyline | you are my lifeline | ride the bassline | on the frontline | make the headline | we go online | feel the bassline | follow the bassline
paint the town | tear it down | shut it down | wear the crown | slow it down | we don't back down | burn down the town
last dance | take your chance | lost in a trance | this is our chance | one more dance | one last dance
we revive | let it thrive | feel so alive | we thrive | watch us thrive
all along | sing your song | can't go wrong | dance along | we are strong | nothing is wrong | hear our song
under the sun | we have just begun | second to none | till the morning sun | having fun | we are not done | the night is young and fun
work of art | off the chart | play my part | here we start | open your heart | feel the heart | so smart | top of the chart
"""

# heads: line openers (any ending). "_" is a one-slot rest (syncopation).
HEADS = """
move your body | feel the bass | hands up | let's go | pump it up | get down | to the rhythm | we rise | don't stop | turn it up
feel the heat | lose control | one more time | here we go | make some noise | light it up | shake it | move it | dance all night
feel alive | to the beat | on the floor | put your hands up | can you feel it | we own the night | ride the wave | catch the vibe
burn the floor | drop the bass | bring the heat | the night is young | bounce | work it | let it go | kick the drum | follow the light
hear the sound | lose your mind | set me free | come alive | fly so high | touch the sky | we go higher | feel the fire | we don't stop
yeah | oh yeah | come on | let's dance | all together | louder | faster | harder | stronger | deeper | right now | this is it
no sleep | wake up | break it down | turn around | get up | stand up | rise up | move to the beat | dance with me | stay with me
take control | feel the groove | in the mix | electric night | party people | to the top | one two three | three two one | bass drop
shine bright | like a star | we are alive | dj turn it up | we are the music | nothing can stop us | everybody | tonight | higher
all night long | put your hands in the air | jump _ jump _ | pump _ pump _ | up _ up _ | go _ go _ | oh _ oh _ | hey _ hey _
feel it _ | move _ move | clap _ clap _ | dance _ dance _ | music | party | freedom | power | we came to dance | feel the power
everybody on the floor | we came here to dance | turn the music up | feel it in your soul | hands up in the air | do you feel the bass
let the music play | music take control | come on everybody | jump _ everybody | turn up the bass | we never stop | light up the night
faster _ faster _ | louder _ louder _ | higher _ higher _ | all the party people | everybody move | we dance all night | move your body now
lights out | arms up | heads up | eyes closed | get ready | are you ready | here it comes | this is the moment | we go hard | push it | ride it
feel it now | hands in the sky | feet on the floor | eyes on me | hey dj | dj play it | lift me up | hold me close | don't let go | never let go
all in | go all in | never back down | step into the light | we ride | we burn | we shine | we glow | run it back | play it back
one more drop | the drop | drop it low | low low low | up up up | round and round | over and over | again and again | louder and louder
keep it moving | keep it going | keep on dancing | keep the fire | catch the fire | neon lights | city lights | summer nights | friday night
saturday night | all weekend | after hours | from dusk till dawn | heartbeat racing | my body | your body | lost in the music | lost in the moment
lost in the lights | feel the thunder | hear the thunder | like lightning | like a rocket | like a bullet | shoot for the stars | reach for the stars
we are stars | superstar | rock star | shining star | born to dance | born to run | born to shine | made to move | here to party | here to dance
in the club | center stage | main stage | big room | dark room | underground | deep down | way down | way up | sky high | too high | fly high
take off | lift off | blast off | countdown | ten nine eight | set it off | kick it off | start it up | fire it up | let it loose | break loose
all out | go all out | push it to the max | no limits | no rules | no fear | can't stop won't stop | we won't stop | nonstop party | party all night
day and night | every night | every day | every beat | every drop | one love | one sound | one beat | one night | tonight we dance | tonight we fly
tonight we rise | tonight we shine | this is the night | this is our night | this is the sound | we do it | we did it | we made it | we own it
own the floor | own the sound | rule the night | run the night | run the floor | run the game | game on | lights on | sound on | volume up
turn the volume up | crank it up | pump the volume | drop it | hot hot hot | it's hot | too hot | feel the fever | night fever | dance fever
disco fever | disco lights | disco night | techno night | house music | deep house | tech house | we love house | house all night | bass all night
in the house | raise the roof | hit the roof | through the roof | off the wall | back to back | wall to wall | the beat goes on | music all night
music is life | music is the answer | let the beat drop | rhythm of the night | rhythm in my soul | soul on fire | heart on fire | mind on fire
we on fire | fire fire fire | burn burn burn | glow glow glow | dance dance dance | up and down | in and out | back and forth | side to side
left right left | front to back | top to bottom | low to high | slow to fast | soft to loud | whisper to a scream | hear the drums | feel the drums
we are the drums | ooh _ ooh _ | ah _ ah _ | la la la | na na na | da da da | bam bam | pow _ pow _ | bang bang | hot _ hot _
"""

# one-shot shouts (single words)
SHOUTS = ("jump higher tonight everybody fire louder bounce yeah go dance move freedom power party music bass drop rise "
          "hey oh alive up down stop rhythm electric vibration celebration sensation paradise energy "
          "gold wild dream scream team queen deep rush crush grind ghost fade raise blaze lift drift flip whip blow throw grow "
          "stone phone brain chain train spray pray lit hit boom zoom bloom moon tune swing ring king thing wing spring sting "
          "bling drip breeze freeze squeeze please spark dark glass class more core score roar soar storm warm born horn cool "
          "pool rule school choose cruise ride wide slide glide pride tide sunrise sunset moonlight starlight midnight daylight "
          "spotlight heartbeat sunshine echo tempo disco techno vinyl neon laser thunder whisper crazy pressure pleasure treasure "
          "heaven seven wonder summer season reason kingdom system vision mission anthem rocket sonic magic skyline lifeline "
          "bassline revolution evolution overload overdrive afterglow tomorrow hurricane volcano dynamite satellite carnival "
          "festival magical musical gravity galaxy fantasy destiny harmony symphony memory victory history mystery liberty "
          "euphoria wonderful beautiful unstoppable incredible invincible unbreakable adrenaline superstar underground "
          "countdown lightning eternal infinite ooh whoa bam pow bang zap")
# one-shot short phrases (2..3 words, <= 4 slots)
SHOUT_PHRASES = ("hands up | let's go | pump it up | move your body | feel the bass | get down | all night long | to the rhythm "
                 "| we rise | don't stop | turn it up | here we go | make some noise | come on | oh yeah | feel the heat "
                 "| lose control | one more time | let's dance | bass drop | wake up | rise up | light it up | shake it "
                 "| feel alive | to the beat | so high | we fly | so loud | right now | drop the bass | feel the fire "
                 "| lights out | arms up | heads up | eyes closed | get ready | are you ready | here it comes | we go hard | push it "
                 "| ride it | feel it now | hey dj | dj play it | lift me up | don't let go | never let go | all in | we ride | we burn "
                 "| we shine | we glow | run it back | play it back | one more drop | drop it low | up up up | neon lights | city lights "
                 "| summer nights | friday night | after hours | my body | your body | like lightning | like a rocket | rock star "
                 "| shining star | born to dance | born to run | born to shine | made to move | here to party | in the club | main stage "
                 "| big room | deep down | way up | sky high | take off | lift off | blast off | set it off | kick it off | start it up "
                 "| fire it up | let it loose | break loose | all out | no limits | no rules | no fear | we won't stop | nonstop party "
                 "| party all night | day and night | every beat | one love | one sound | one beat | game on | lights on | volume up "
                 "| crank it up | drop it | it's hot | too hot | night fever | disco lights | house music | deep house | we love house "
                 "| raise the roof | through the roof | back to back | wall to wall | the beat goes on | let the beat drop | soul on fire "
                 "| heart on fire | we on fire | fire fire fire | burn burn burn | glow glow glow | dance dance dance | up and down "
                 "| round and round | in and out | side to side | left right left | feel the rush | feel the thunder | hear the thunder "
                 "| free your mind | to the moon | hear the boom | boom boom boom | zoom zoom zoom | bling bling | light the spark "
                 "| in the dark | give me more | more more more | hit the floor | stay cool | old school | worldwide | side by side "
                 "| come inside | set the tone | take the throne | ride the train | watch it grow | after midnight | in the moonlight "
                 "| live forever | dance forever | now or never | all together | we follow | pure ecstasy | on the skyline "
                 "| ride the bassline | paint the town | tear it down | shut it down | wear the crown | last dance | one more dance "
                 "| sing your song | under the sun | work of art | off the chart | open your heart | hot hot hot | la la la | na na na")


def parse_bank(text):
    out = []
    for line in text.strip().splitlines():
        for p in line.split("|"):
            p = p.strip()
            if p:
                out.append(p)
    return out


def tokens(phrase):
    return phrase.split()


def word_slots(w):
    if w == "_":
        return 1
    return 1 if SYL[w] == 1 else 2


def phrase_slots(p):
    return sum(word_slots(w) for w in tokens(p))


def tail_family(p):
    return FAMILY_OF[tokens(p)[-1]]


def check_lexicon():
    missing = set()
    for bank in (TAILS, HEADS, SHOUT_PHRASES):
        for p in parse_bank(bank):
            for w in tokens(p):
                if w != "_" and w not in SYL:
                    missing.add(w)
    for w in SHOUTS.split():
        if w not in SYL:
            missing.add(w)
    for p in parse_bank(TAILS):
        if tokens(p)[-1] not in FAMILY_OF:
            missing.add("(no family) " + p)
    if missing:
        sys.exit("lexicon error, missing syllable counts / families: " + ", ".join(sorted(missing)))


# ------------------------------------------------------------------------------------------------ lyrics

class Line:
    """One bar: words with slot positions. items = [[slot, word, slots]] (rests are not stored)."""
    def __init__(self, words):
        self.items = []
        s = 0
        for w in words:
            n = word_slots(w)
            if w != "_":
                self.items.append([s, w, n])
            s += n
        self.used = s
        if self.items and self.items[-1][2] == 1 and self.used < SLOTS_PER_BAR:   # the last word breathes
            self.items[-1][2] = 2
            self.used += 1
        assert self.used <= SLOTS_PER_BAR, words

    def text(self):
        return " ".join(w for _, w, _ in self.items)

    def words(self):
        return [w for _, w, _ in self.items]


class Lyrics:
    def __init__(self, rng):
        self.rng = rng
        self.tails = parse_bank(TAILS)
        self.heads = parse_bank(HEADS)
        self.by_family = {}
        for t in self.tails:
            self.by_family.setdefault(tail_family(t), []).append(t)
        self.shouts = SHOUTS.split()
        self.shout_phrases = parse_bank(SHOUT_PHRASES)

    def line(self, tail, used_heads):
        t = phrase_slots(tail)
        target = self.rng.choices([8, 7, 6], weights=[50, 35, 15])[0]
        for want in (target, 8, 7, 6):
            need = want - t
            cands = [h for h in self.heads if phrase_slots(h) == need and h not in used_heads
                     and tokens(h)[-1] != tokens(tail)[-1]]
            if cands:
                h = self.rng.choice(cands)
                used_heads.add(h)
                return Line(tokens(h) + tokens(tail))
        return Line(tokens(tail))

    def couplet(self, family, used_heads):
        pool = self.by_family[family]
        a = self.rng.choice(pool)
        others = [t for t in pool if tokens(t)[-1] != tokens(a)[-1]]
        b = self.rng.choice(others if others else [t for t in pool if t != a] or pool)
        return [self.line(a, used_heads), self.line(b, used_heads)]

    def families(self, n):
        fams = [f for f, ts in self.by_family.items() if len({tokens(t)[-1] for t in ts}) >= 2]
        return self.rng.sample(fams, n)

    def one_shot(self):
        if self.rng.random() < 0.4:
            return Line([self.rng.choice(self.shouts)]), "shout"
        return Line(tokens(self.rng.choice(self.shout_phrases))), "phrase"

    def loop(self):
        used = set()
        if self.rng.random() < 0.5:
            return self.couplet(self.families(1)[0], used), "couplet"
        ln = self.line(self.rng.choice(self.tails), used)
        return [ln, ln], "line x2"

    def session(self):
        used = set()
        lines = []
        for fam in self.families(4):
            lines += self.couplet(fam, used)
        return lines, "4 couplets"


def slugify(words, limit=24):
    parts = [re.sub(r"[^A-Za-z0-9]", "", w) for w in words]
    parts = [p for p in parts if p]
    slug = ""
    for p in parts:
        cand = (slug + "_" + p) if slug else p
        if len(cand) > limit:
            break
        slug = cand
    if not slug:
        slug = parts[0][:limit] if parts else "rap"
    return slug


# ------------------------------------------------------------------------------------------------ audio helpers

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, check=True, **kw)


def ffmpeg_pcm(src):
    cmd = ["ffmpeg", "-v", "error", "-i", src, "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "-"]
    data = run(cmd).stdout
    a = array("h")
    a.frombytes(data[: len(data) - len(data) % 2])
    return a


def pcm_filter(a, filters):
    """Run ffmpeg filters over raw PCM held in memory (stdin -> stdout)."""
    cmd = ["ffmpeg", "-v", "error", "-f", "s16le", "-ar", str(RATE), "-ac", "1", "-i", "pipe:0", "-af", filters,
           "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "pipe:1"]
    data = run(cmd, input=a.tobytes()).stdout
    out = array("h")
    out.frombytes(data[: len(data) - len(data) % 2])
    return out


def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == RATE, path
        a = array("h")
        a.frombytes(w.readframes(w.getnframes()))
    return a


def write_wav(path, a):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with wave.open(tmp, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(a.tobytes())
    os.replace(tmp, path)


def db(x):
    return 20 * math.log10(max(x, 1e-9))


def peak_of(a):
    return max(1, max(abs(x) for x in a)) / 32768.0 if len(a) else 1 / 32768.0


def rms_db_loudest(a, window=int(RATE * 0.02)):
    if len(a) < window:
        window = max(1, len(a))
    acc = 0
    for i in range(window):
        acc += a[i] * a[i]
    best = acc
    for i in range(window, len(a)):
        acc += a[i] * a[i] - a[i - window] * a[i - window]
        if acc > best:
            best = acc
    return db(math.sqrt(best / window) / 32768.0)


def trim(a, thr_db=TRIM_DB, pre_ms=2.0):
    thr = int(32767 * 10 ** (thr_db / 20))
    start = 0
    while start < len(a) and abs(a[start]) < thr:
        start += 1
    start = max(0, start - int(RATE * pre_ms / 1000))
    end = len(a)
    while end > start and abs(a[end - 1]) < thr // 2:
        end -= 1
    return a[start:end]


def segments(a, thr_db=-50.0, min_gap_ms=300, min_len_ms=60):
    """Loud regions separated by at least min_gap_ms of quiet (for splitting a [[slnc]]-separated batch)."""
    thr = int(32767 * 10 ** (thr_db / 20))
    gap = int(RATE * min_gap_ms / 1000)
    segs, start, last = [], None, -10 ** 9
    for i, x in enumerate(a):
        if abs(x) >= thr:
            if start is None:
                start = i
            last = i
        elif start is not None and i - last > gap:
            segs.append((start, last + 1))
            start = None
    if start is not None:
        segs.append((start, last + 1))
    return [s for s in segs if s[1] - s[0] >= RATE * min_len_ms / 1000]


def apply_gain_fades(a, gain, fade_in_ms=3, fade_out_ms=10):
    n = len(a)
    fi = min(n // 2, int(RATE * fade_in_ms / 1000))
    fo = min(n // 2, int(RATE * fade_out_ms / 1000))
    for i in range(n):
        g = gain
        if i < fi:
            g *= i / fi
        if i >= n - fo:
            g *= (n - i) / fo
        a[i] = max(-32768, min(32767, int(a[i] * g)))
    return a


def pitch_track(a, fmin, fmax, frame=1024, hop=256, thr=0.15, min_db=-32.0):
    """YIN f0 track of 16-bit PCM on 2:1 decimated audio: [(time_s, f0_hz)] for voiced frames (cumulative-mean
    normalised difference below `thr`, else its minimum when below 0.3), parabolic lag interpolation."""
    rate = RATE // 2
    d = [float(a[i] + a[i + 1]) * 0.5 for i in range(0, len(a) - 1, 2)]
    w = frame // 2
    lo, hi = max(2, int(rate / fmax)), min(w, int(rate / fmin))
    ethr = (32768 * 10 ** (min_db / 20)) ** 2
    out = []
    for s in range(0, len(d) - frame, hop):
        x = d[s:s + frame]
        head = x[:w]
        e0 = sum(map(mul, head, head))
        if e0 / w < ethr:
            continue
        cm, run_sum = [], 0.0
        for tau in range(1, hi + 1):
            seg = x[tau:tau + w]
            diff = e0 + sum(map(mul, seg, seg)) - 2 * sum(map(mul, head, seg))
            run_sum += diff
            cm.append(diff * tau / run_sum if run_sum > 0 else 1.0)
        pick = None
        for i in range(lo - 1, hi):
            if cm[i] < thr:
                while i + 1 < hi and cm[i + 1] < cm[i]:
                    i += 1
                pick = i
                break
        if pick is None:
            m = min(range(lo - 1, hi), key=lambda i: cm[i])
            if cm[m] > 0.3:
                continue
            pick = m
        tau = pick + 1.0
        if 0 < pick < len(cm) - 1:
            y0, y1, y2 = cm[pick - 1], cm[pick], cm[pick + 1]
            den = y0 - 2 * y1 + y2
            if den > 0:
                tau += 0.5 * (y0 - y2) / den
        out.append((s / rate, rate / tau))
    return out


def core_midi(track):
    """The word's pitch as MIDI: median of the voiced frames between 10 % and 60 % of the voiced span, so the
    final fall the voices put on every word (and onset noise) does not count. (None, 0) without voiced frames."""
    if not track:
        return None, 0
    n = len(track)
    if n >= 4:
        core = track[int(n * 0.1): max(int(n * 0.1) + 1, int(math.ceil(n * 0.6)))]
    else:
        core = track[: max(1, n - 1)]
    vals = sorted(midi_of(f) for _, f in core)
    return vals[len(vals) // 2], n


def midi_of(f):
    return 69 + 12 * math.log2(f / 440.0)


def note_name(m):
    return ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][int(round(m)) % 12] + str(int(round(m)) // 12 - 1)


# ------------------------------------------------------------------------------------------------ say rendering

def voice_slug(v):
    return re.sub(r"[^A-Za-z0-9]+", "", v)


def say_to_pcm(voice, text, rate):
    with tempfile.NamedTemporaryFile(suffix=".aiff", delete=False) as tf:
        path = tf.name
    try:
        run(["say", "-v", voice, "-r", str(rate), "-o", path, text])
        return ffmpeg_pcm(path)
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


def say_words(voice, words, rate, pbas=None, slnc_ms=600):
    """Render `words` (each as its own sentence, separated by [[slnc]]) in ONE say call and split on the pauses.
    Falls back to one call per word when the split does not yield one segment per word."""
    pre = f"[[pbas {pbas}]] " if pbas is not None else ""
    text = pre + " ".join(f"{w}. [[slnc {slnc_ms}]]" for w in words)
    a = say_to_pcm(voice, text, rate)
    segs = segments(a)
    if len(segs) > len(words):                  # a click or breath in a pause: drop the shortest surplus segments
        surplus = sorted(segs, key=lambda s: s[1] - s[0])[: len(segs) - len(words)]
        if all(e - s < RATE * 0.12 for s, e in surplus):
            segs = [s for s in segs if s not in surplus]
    if len(segs) == len(words):
        return {w: trim(a[s:e]) for w, (s, e) in zip(words, segs)}
    out = {}
    for w in words:
        out[w] = trim(say_to_pcm(voice, f"{pre}{w}.", rate))
    out["__fallback__"] = len(segs)
    return out


class Cache:
    def __init__(self, root, rate):
        self.root = root
        self.rate = rate
        for sub in ("words", "fit", "calib"):
            os.makedirs(os.path.join(root, sub), exist_ok=True)

    def word_path(self, voice, pbas, word):
        return os.path.join(self.root, "words", voice_slug(voice), f"p{pbas if pbas is not None else 'na'}_r{self.rate}",
                            slugify([word]) + ".wav")

    def fit_path(self, voice, pbas, word, shift, budget):
        return os.path.join(self.root, "fit", voice_slug(voice), f"p{pbas if pbas is not None else 'na'}_r{self.rate}",
                            f"{slugify([word])}_s{shift:+.1f}_{budget}.wav")

    def calib_path(self, voice):
        return os.path.join(self.root, "calib", f"{voice_slug(voice)}_r{self.rate}.json")


def render_missing(cache, voice, pbas, words, batch=40, log=print):
    missing = [w for w in words if not os.path.exists(cache.word_path(voice, pbas, w))]
    if not missing:
        return 0, 0
    calls = 0
    fallbacks = 0
    for i in range(0, len(missing), batch):
        chunk = missing[i:i + batch]
        res = say_words(voice, chunk, cache.rate, pbas)
        calls += 1
        if "__fallback__" in res:
            fallbacks += 1
            log(f"    note: {voice} pbas {pbas}: batch of {len(chunk)} split into {res.pop('__fallback__')} segments, rendered word by word")
        for w, a in res.items():
            if len(a) < RATE * 0.03:
                log(f"    warning: {voice} pbas {pbas}: '{w}' rendered to {len(a) / RATE * 1000:.0f} ms")
            write_wav(cache.word_path(voice, pbas, w), a)
    return calls, fallbacks


def calibrate(cache, voice, f0_range, log=print):
    """pbas -> measured core MIDI (median over CALIB_WORDS); cached as JSON. Returns a sorted list of (pbas, midi)."""
    p = cache.calib_path(voice)
    if os.path.exists(p):
        with open(p) as f:
            return [tuple(x) for x in json.load(f)["table"]]
    table = []
    for pb in CALIB_PBAS:
        res = say_words(voice, CALIB_WORDS, cache.rate, pb)
        res.pop("__fallback__", None)
        mids = []
        for w, a in res.items():
            m, n = core_midi(pitch_track(a, *f0_range))
            if m is not None and n >= 2:
                mids.append(m)
        if mids:
            mids.sort()
            table.append((pb, round(mids[len(mids) // 2], 2)))
    mono = []                                   # monotone (running max) so the inverse is well defined
    hi = -1e9
    for pb, m in table:
        hi = max(hi, m)
        mono.append((pb, round(hi, 2)))
    with open(p, "w") as f:
        json.dump({"voice": voice, "rate": cache.rate, "words": CALIB_WORDS, "table": mono}, f, indent=1)
    log(f"  calibrated {voice}: pbas {mono[0][0]}..{mono[-1][0]} -> MIDI {mono[0][1]}..{mono[-1][1]}")
    return mono


def pbas_for(table, target_midi):
    """Inverse of the calibration table (piecewise linear, clamped)."""
    if target_midi <= table[0][1]:
        return table[0][0]
    for (p0, m0), (p1, m1) in zip(table, table[1:]):
        if m0 <= target_midi <= m1:
            if m1 == m0:
                return p0
            return int(round(p0 + (p1 - p0) * (target_midi - m0) / (m1 - m0)))
    return table[-1][0]


def target_note(root, centre, table):
    """The root's MIDI note nearest `centre` that the voice reaches (calibrated range widened by the tune range)."""
    pc = PC[root]
    cands = sorted((abs(pc + 12 * k - centre), pc + 12 * k) for k in range(0, 11))
    lo, hi = table[0][1] - TUNE_MAX_ST, table[-1][1] + TUNE_MAX_ST
    for _, m in cands:
        if lo <= m <= hi:
            return m
    return cands[0][1]


# ------------------------------------------------------------------------------------------------ word fitting

def measure_word(args):
    """Worker: core pitch (MIDI) of a cached raw word, or None."""
    key, src, f0_range = args
    m, n = core_midi(pitch_track(trim(read_wav(src)), *f0_range))
    return key, (m if (m is not None and n >= 2) else None)


def fit_word(args):
    """Worker: raw cached word -> levelled, pitch-shifted by `shift` semitones, slot-fitted word. Returns stats."""
    src, dst, budget, shift, allow_stretch = args
    if os.path.exists(dst):
        return None
    a = trim(read_wav(src))
    if len(a) < RATE * 0.02:
        a = array("h", [0] * int(RATE * 0.02))
    dur = len(a)
    f = 2 ** (shift / 12)                       # asetrate factor: pitch up by `shift` semitones, duration / f
    final = min(dur, int(budget * 0.97))        # aim a little under the budget: atempo overshoots by a few ms
    filters = []
    if abs(shift) > 0:
        filters.append(f"asetrate={RATE * f:.3f}")
        filters.append(f"aresample={RATE}")
    tempo = (dur / f) / final                    # > 1 compresses
    if abs(shift) > 0 or tempo > 1.001:
        if tempo < 0.5:
            tempo = 0.5
        if tempo > 2.0 and not allow_stretch:
            tempo = 2.0
        while tempo > 2.0:
            filters.append("atempo=2.0")
            tempo /= 2.0
        if abs(tempo - 1.0) > 0.001:
            filters.append(f"atempo={tempo:.4f}")
    if filters:
        a = pcm_filter(a, ",".join(filters))
        a = trim(a, pre_ms=1.0)
    cut = len(a) - budget
    truncated = cut > RATE * 0.005
    if cut > 0:
        a = a[:budget]
    rms = rms_db_loudest(a)
    gain = 10 ** ((WORD_RMS_DB - rms) / 20)
    gain = min(gain, 10 ** (PEAK_DB / 20) / peak_of(a))
    apply_gain_fades(a, gain, fade_out_ms=20 if truncated else 10)
    write_wav(dst, a)
    return {"dur_ms": dur * 1000 // RATE, "fit_ms": len(a) * 1000 // RATE, "tempo": round(tempo, 2), "shift": shift, "truncated": truncated}


# ------------------------------------------------------------------------------------------------ products

class Product:
    def __init__(self, kind, bars, vclass, voice, key, lines, note):
        self.kind, self.bars, self.vclass, self.voice, self.key, self.lines, self.note = kind, bars, vclass, voice, key, lines, note
        self.slug = self.name = self.path = None
        self.plan = {"target": None, "pbas": None}
        self.shifts = {}                        # word -> fine-tune shift (semitones) used for this product

    def f0_range(self):
        for v, _, r in VOICES.get(self.vclass, []):
            if v == self.voice:
                return r
        return (60, 500)

    def placements(self):
        """[(sample, bar, slot, word, slots, budget)] over the whole product."""
        out = []
        for b, ln in enumerate(self.lines):
            for slot, w, n in ln.items:
                budget = int(n * SLOT * SLOT_FILL)
                if self.kind == "oneshot" and len(ln.items) == 1:
                    budget = int(4 * SLOT * SLOT_FILL)                             # a lone shout may ring up to 4 slots
                    if self.voice in NOVELTY:
                        budget = int(SLOTS_PER_BAR * SLOT * SLOT_FILL)             # singing voices get the bar
                out.append(((b * SLOTS_PER_BAR + slot) * SLOT, b, slot, w, n, budget))
        return out


PLAN_RETRIES = 80                               # lyric draws per product before a repeat / slug suffix is accepted
PLAN_STATS = {"lines": 0, "repeated": 0, "slug_collisions": 0, "products": 0}


class Dedupe:
    """Per key folder: the line texts already used (across M / F / R) and the slugs taken per (class, bars), so a
    folder never holds the same line twice and no name needs a `_2` suffix (research §3: 24 % repeats and 46
    collisions at 315 files per key with the old 63 one-shot texts). draw() retries the lyric generator."""
    def __init__(self):
        self.lines = {}                         # folder -> set(line text)
        self.slugs = set()                      # (folder, class, bars, slug)

    def draw(self, make, kind, folder, vclass, bars):
        used = self.lines.setdefault(folder, set())
        lines = note = texts = slug = None
        for attempt in range(PLAN_RETRIES):
            lines, note = make()
            if kind == "oneshot":
                lines = [lines]
            texts = {ln.text() for ln in lines}
            slug = slugify(lines[0].words())
            if texts.isdisjoint(used) and (folder, vclass, bars, slug) not in self.slugs:
                break
        PLAN_STATS["products"] += 1
        PLAN_STATS["lines"] += len(texts)
        PLAN_STATS["repeated"] += len(texts & used)
        if (folder, vclass, bars, slug) in self.slugs:
            PLAN_STATS["slug_collisions"] += 1
            base = slug
            k = 2
            while (folder, vclass, bars, slug) in self.slugs:
                suffix = f"_{k}"
                slug = base[: 24 - len(suffix)].rstrip("_") + suffix
                k += 1
        used |= texts
        self.slugs.add((folder, vclass, bars, slug))
        return lines, note, slug


def plan_products(args, rng_seed):
    """Deterministic plan: every product of every key, with its voice and lyrics. Lines are unique within a key
    folder and slugs within (folder, class, bars) as far as PLAN_RETRIES draws allow (PLAN_STATS counts the rest)."""
    for k in PLAN_STATS:
        PLAN_STATS[k] = 0
    products = []
    dd = Dedupe()
    key_list = [k for k in args.keys] if args.keys else KEYS
    for ki, root in enumerate(key_list):
        folder = root + "min"
        for vclass, counts in (("M", (args.oneshots_mf, args.loops_mf, args.sessions_mf)),
                               ("F", (args.oneshots_mf, args.loops_mf, args.sessions_mf)),
                               ("R", (args.oneshots_r, args.loops_r, args.sessions_r))):
            rng = random.Random(f"{rng_seed}:{folder}:{vclass}")
            ly = Lyrics(rng)
            voices = [v for v, _, _ in VOICES[vclass]]
            n = 0
            for kind, bars, count, make in (("oneshot", 1, counts[0], ly.one_shot), ("loop", 2, counts[1], ly.loop), ("session", 8, counts[2], ly.session)):
                for i in range(count):
                    voice = voices[(n + ki) % len(voices)]
                    n += 1
                    lines, note, slug = dd.draw(make, kind, folder, vclass, bars)
                    pr = Product(kind, bars, vclass, voice, folder, lines, note)
                    pr.slug = slug
                    products.append(pr)
    if args.na_oneshots or args.na_loops or args.na_sessions:
        rng = random.Random(f"{rng_seed}:NA:R")
        ly = Lyrics(rng)
        robots = [v for v, _, _ in VOICES["R"]]
        for i in range(args.na_oneshots):
            j = i - (args.na_oneshots - 2)
            if 0 <= j < len(NOVELTY):                                             # the last two: singing voices, one word
                voice = NOVELTY[j]
                lines, note, slug = dd.draw(lambda: (Line([rng.choice(ly.shouts)]), "sung shout"), "oneshot", "NA", "R", 1)
            else:
                voice = robots[i % len(robots)]
                lines, note, slug = dd.draw(ly.one_shot, "oneshot", "NA", "R", 1)
            pr = Product("oneshot", 1, "R", voice, "NA", lines, note)
            pr.slug = slug
            products.append(pr)
        for i in range(args.na_loops):
            lines, note, slug = dd.draw(ly.loop, "loop", "NA", "R", 2)
            pr = Product("loop", 2, "R", robots[i % len(robots)], "NA", lines, note)
            pr.slug = slug
            products.append(pr)
        for i in range(args.na_sessions):
            lines, note, slug = dd.draw(ly.session, "session", "NA", "R", 8)
            pr = Product("session", 8, "R", robots[i % len(robots)], "NA", lines, note)
            pr.slug = slug
            products.append(pr)
    for p in products:
        p.name = f"{p.key}__{BPM}bpm__RAP{p.vclass}__{p.bars}bars__{p.slug}.wav"
        assert NAME_RE.match(p.name), p.name
    return products


def plan_report():
    st = PLAN_STATS
    pct = 100.0 * st["repeated"] / max(1, st["lines"])
    return (f"plan: {st['products']} products, {st['lines']} lines; {st['repeated']} repeated within a key folder "
            f"({pct:.1f} %), {st['slug_collisions']} slug collisions ({PLAN_RETRIES} draws per product)")


def dest_folder(vocal_root, key):
    folder = os.path.join(vocal_root, key)
    if os.path.isdir(folder):
        subdirs = [d for d in os.listdir(folder) if os.path.isdir(os.path.join(folder, d)) and not d.startswith(".")]
        if subdirs:
            return os.path.join(folder, "rap")      # the key folder is a shard root: stay out of its leaf rule
    return folder


def locate_existing(products, vocal_root):
    """Point every product at the file that carries its name under its key folder (shard_folders.py may have
    moved it into an sNN subfolder since it was written); products without a file keep their planned path."""
    by_key = {}
    for p in products:
        by_key.setdefault(p.key, []).append(p)
    for key, ps in by_key.items():
        found = {}
        for dp, _, fns in os.walk(os.path.join(vocal_root, key)):
            for f in fns:
                found.setdefault(f, os.path.join(dp, f))
        for p in ps:
            if not os.path.exists(p.path) and p.name in found:
                p.path = found[p.name]


def remove_existing(vocal_root, log):
    n = 0
    for dp, dns, fns in os.walk(vocal_root):
        for f in fns:
            if RAP_GLOB_RE.search(f):
                os.remove(os.path.join(dp, f))
                n += 1
    if n:
        log(f"removed {n} earlier RAP files under {vocal_root}")
    return n


# ------------------------------------------------------------------------------------------------ verification

def detect_onsets(a, on_db=-36.0, off_db=-48.0, quiet_ms=25, win_ms=4):
    """Onset sample indices: the short-window RMS rises above on_db after >= quiet_ms below off_db."""
    win = int(RATE * win_ms / 1000)
    on = (32768 * 10 ** (on_db / 20)) ** 2
    off = (32768 * 10 ** (off_db / 20)) ** 2
    quiet_need = int(RATE * quiet_ms / 1000) // win
    onsets, quiet, armed = [], quiet_need, True
    for s in range(0, len(a) - win, win):
        e = 0
        for i in range(s, s + win):
            e += a[i] * a[i]
        e /= win
        if e < off:
            quiet += 1
            if quiet >= quiet_need:
                armed = True
        elif e >= on and armed:
            onsets.append(s)
            armed = False
            quiet = 0
        else:
            quiet = 0
    return onsets


def pitch_check(args):
    """Worker: core pitch of every placed word of one output file vs its target. Returns [(word, dev_st)]."""
    path, placements, f0_range, target = args
    a = read_wav(path)
    out = []
    for start, _, _, w, _, budget in placements:
        m, n = core_midi(pitch_track(a[start:start + budget], *f0_range))
        if m is not None and n >= 2:
            out.append((w, m - target))
    return out


def verify(products, log, jobs=4, grid_samples=4, rng_seed=0):
    """Listening-free checks over the written files. Returns the number of hard failures."""
    fails = durs_ok = 0
    peaks, grid_dev = [], []
    rng = random.Random(rng_seed)
    printed_grid = 0
    log("\nverification")
    for p in products:
        if not p.path or not os.path.exists(p.path):
            log(f"  FAIL missing {p.name}")
            fails += 1
            continue
        base = os.path.basename(p.path)
        m = NAME_RE.match(base)
        if not m or int(m.group("bpm")) != BPM or int(m.group("bars")) != p.bars or len(base.encode()) > 95:
            log(f"  FAIL name does not parse: {p.name}")
            fails += 1
        a = read_wav(p.path)
        want = p.bars * BAR
        if abs(len(a) - want) > RATE * 0.02:
            log(f"  FAIL duration {len(a) / RATE:.3f}s != {want / RATE:.3f}s: {p.name}")
            fails += 1
        else:
            durs_ok += 1
        pk = db(peak_of(a))
        peaks.append(pk)
        if abs(pk - PEAK_DB) > 0.2:
            log(f"  FAIL peak {pk:.2f} dBFS: {p.name}")
            fails += 1
        onsets = detect_onsets(a)                              # every onset should sit on a planned word start
        planned = [pl[0] for pl in p.placements()]
        for o in onsets:
            nearest = min(planned, key=lambda s: abs(s - o))
            grid_dev.append((o - nearest) / RATE * 1000)
        if printed_grid < grid_samples and p.kind != "oneshot" and rng.random() < 0.12:
            printed_grid += 1
            log(f"  grid  {p.name}")
            log("        planned " + " ".join(f"{pl[1]}.{pl[2]}:{pl[3]}" for pl in p.placements()[:12]))
            log("        onsets  " + " ".join(f"{o / RATE:.3f}" for o in onsets[:12]) + f"   ({len(onsets)} onsets / {len(planned)} words)")
    gd = sorted(abs(x) for x in grid_dev)
    within = sum(1 for x in gd if x <= 30)
    log(f"  {durs_ok}/{len(products)} durations exact (bars x 1.92 s); peaks {min(peaks):.2f}..{max(peaks):.2f} dBFS; "
        f"{len(products)} names parse")
    log(f"  grid: {len(gd)} detected onsets, {within} ({100 * within / max(1, len(gd)):.0f} %) within 30 ms of a planned 1/8 slot; "
        f"|dev| median {gd[len(gd) // 2] if gd else 0:.1f} ms, p95 {gd[int(len(gd) * 0.95)] if gd else 0:.1f} ms (late onsets are soft consonants)")
    # pitch: every word of every M/F file, measured in the output file
    tuned = [p for p in products if p.vclass in ("M", "F") and p.plan["target"] is not None and p.path and os.path.exists(p.path)]
    jobs_list = [(p.path, p.placements(), p.f0_range(), p.plan["target"]) for p in tuned]
    devs = {"M": [], "F": []}
    per_file = []
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        for p, res in zip(tuned, ex.map(pitch_check, jobs_list, chunksize=4)):
            devs[p.vclass] += [d for _, d in res]
            if res:
                per_file.append((p, sorted(d for _, d in res)[len(res) // 2]))
    log("  pitch: core pitch of each voiced word in the output vs the key root (semitones, + = sharp)")
    for c, ds in devs.items():
        if not ds:
            continue
        ad = sorted(abs(d) for d in ds)
        log(f"    {c}: {len(ds)} words, mean {sum(ds) / len(ds):+.2f} st, |dev| median {ad[len(ad) // 2]:.2f}, "
            f"{100 * sum(1 for d in ad if d <= 1.0) / len(ad):.0f} % within 1 st, {100 * sum(1 for d in ad if d <= 2.0) / len(ad):.0f} % within 2 st, max {ad[-1]:.1f} st")
    off = [(p, d) for p, d in per_file if abs(d) > 1.5]
    for p, d in sorted(off, key=lambda x: -abs(x[1]))[:6]:
        log(f"    off  {d:+5.1f} st  {p.name}  ({p.voice}, target {note_name(p.plan['target'])}, pbas {p.plan['pbas']})")
    if len(off) > 6:
        log(f"    ... {len(off)} files have a median word deviation above 1.5 st")
    return fails


# ------------------------------------------------------------------------------------------------ main

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default="sdcard", help="SD card root (holds lp/), default sdcard")
    ap.add_argument("--genre", default="House")
    ap.add_argument("--cache", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".rap_cache"))
    ap.add_argument("--keys", default=None, help="comma list of roots, default all 12 (A,E,B,F#,C#,G#,D#,A#,F,C,G,D)")
    ap.add_argument("--seed", default="rap125")
    ap.add_argument("--rate", type=int, default=250, help="say words per minute (250 = one syllable per 1/8 at 125 BPM)")
    ap.add_argument("--oneshots-mf", type=int, default=4)
    ap.add_argument("--loops-mf", type=int, default=3)
    ap.add_argument("--sessions-mf", type=int, default=1)
    ap.add_argument("--oneshots-r", type=int, default=2)
    ap.add_argument("--loops-r", type=int, default=1)
    ap.add_argument("--sessions-r", type=int, default=0)
    ap.add_argument("--na-oneshots", type=int, default=6)
    ap.add_argument("--na-loops", type=int, default=3)
    ap.add_argument("--na-sessions", type=int, default=1)
    ap.add_argument("--no-tune", action="store_true", help="skip the per-word pitch fine-tune (pbas only)")
    ap.add_argument("--jobs", type=int, default=max(2, (os.cpu_count() or 4)))
    ap.add_argument("--keep-existing", action="store_true", help="do not delete earlier RAP files first")
    ap.add_argument("--dry-run", action="store_true", help="print the plan (lyrics, names) and exit")
    ap.add_argument("--verify-only", action="store_true", help="only verify the files of the current plan")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    args.keys = [k.strip() for k in args.keys.split(",")] if args.keys else None
    if args.keys:
        bad = [k for k in args.keys if k not in PC]
        if bad:
            sys.exit("unknown keys: " + ", ".join(bad))
    log = (lambda *a, **k: None) if args.quiet else print
    check_lexicon()
    t0 = time.time()
    products = plan_products(args, args.seed)
    vocal_root = os.path.join(os.path.abspath(args.out), "lp", "packs", args.genre, "vocal")
    for p in products:
        p.path = os.path.join(dest_folder(vocal_root, p.key), p.name)

    if args.dry_run:
        for p in products:
            print(f"{p.name}   [{p.voice}, {p.note}]")
            for ln in p.lines:
                print("    | " + "  ".join(f"{s}:{w}{'(2)' if n == 2 else ''}" for s, w, n in ln.items))
        bars = sum(p.bars for p in products)
        print(f"{len(products)} products, {bars} bars = {bars * BAR * 2 / 1e6:.1f} MB")
        print(plan_report())
        return 0

    cache = Cache(args.cache, args.rate)
    # 1. calibration and targets
    tables = {}
    for vclass in ("M", "F"):
        for voice, centre, f0r in VOICES[vclass]:
            tables[voice] = calibrate(cache, voice, f0r, log)
    for p in products:
        if p.vclass in ("M", "F") and p.key != "NA":
            centre = [c for v, c, _ in VOICES[p.vclass] if v == p.voice][0]
            target = target_note(p.key[:-3], centre, tables[p.voice])
            p.plan = {"target": target, "pbas": pbas_for(tables[p.voice], target)}
    if args.verify_only:
        locate_existing(products, vocal_root)
        return 1 if verify(products, print, jobs=args.jobs, rng_seed=args.seed) else 0

    if not args.keep_existing:
        remove_existing(vocal_root, log)

    # 2. render missing words, batched per (voice, pbas), in parallel over (voice, pbas) groups
    groups = {}
    for p in products:
        for _, _, _, w, _, _ in p.placements():
            groups.setdefault((p.voice, p.plan["pbas"]), set()).add(w)
    nwords = sum(len(ws) for ws in groups.values())
    log(plan_report())
    log(f"{len(products)} products; {nwords} (voice, pitch, word) renders over {len(groups)} say groups")
    t1 = time.time()
    calls = fallbacks = 0
    with cf.ThreadPoolExecutor(max_workers=min(4, args.jobs)) as ex:
        futs = [ex.submit(render_missing, cache, v, pb, sorted(ws), 40, log) for (v, pb), ws in groups.items()]
        for f in futs:
            c, fb = f.result()
            calls += c
            fallbacks += fb
    log(f"  rendered: {calls} say calls ({fallbacks} word-by-word fallbacks) in {time.time() - t1:.1f}s")

    # 3a. measure the core pitch of every tuned word (M/F), then the per-(voice, pbas) median correction
    t2 = time.time()
    measured = {}
    if not args.no_tune:
        todo = {}
        for p in products:
            if p.vclass in ("M", "F") and p.plan["target"] is not None:
                for _, _, _, w, _, _ in p.placements():
                    key = (p.voice, p.plan["pbas"], w)
                    todo[key] = (key, cache.word_path(p.voice, p.plan["pbas"], w), p.f0_range())
        with cf.ProcessPoolExecutor(max_workers=args.jobs) as ex:
            for key, m in ex.map(measure_word, list(todo.values()), chunksize=8):
                measured[key] = m
    bias = {}
    for p in products:
        if p.vclass in ("M", "F") and p.plan["target"] is not None and not args.no_tune:
            g = (p.voice, p.plan["pbas"])
            if g not in bias:
                ds = sorted(p.plan["target"] - m for (v, pb, w), m in measured.items() if v == g[0] and pb == g[1] and m is not None)
                bias[g] = ds[len(ds) // 2] if ds else 0.0
            for _, _, _, w, _, _ in p.placements():
                m = measured.get((p.voice, p.plan["pbas"], w))
                d = (p.plan["target"] - m) if m is not None else bias[g]
                d = max(-TUNE_MAX_ST, min(TUNE_MAX_ST, d))
                p.shifts[w] = 0.0 if abs(d) < 0.25 else round(d, 1)
    if measured:
        nm = sum(1 for m in measured.values() if m is None)
        log(f"  measured {len(measured)} words in {time.time() - t2:.1f}s ({nm} unvoiced, given their group's median correction); "
            "group corrections: " + ", ".join(f"{voice_slug(v)[:5]}@{pb}:{b:+.1f}" for (v, pb), b in sorted(bias.items())[:12]) + (" ..." if len(bias) > 12 else ""))

    # 3b. fit words (level, shift, compress) — one job per distinct (voice, pbas, word, shift, budget)
    t3 = time.time()
    jobs = {}
    for p in products:
        for _, _, _, w, _, budget in p.placements():
            shift = p.shifts.get(w, 0.0)
            dst = cache.fit_path(p.voice, p.plan["pbas"], w, shift, budget)
            if dst not in jobs:
                jobs[dst] = (cache.word_path(p.voice, p.plan["pbas"], w), dst, budget, shift, p.voice in NOVELTY)
    stats = []
    with cf.ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for st in ex.map(fit_word, list(jobs.values()), chunksize=8):
            if st:
                stats.append(st)
    if stats:
        comp = sorted(s["tempo"] for s in stats)
        shifts = sorted(abs(s["shift"]) for s in stats)
        log(f"  fitted {len(stats)} words in {time.time() - t3:.1f}s: atempo median {comp[len(comp) // 2]:.2f}, p90 {comp[int(len(comp) * 0.9)]:.2f}, max {comp[-1]:.2f}; "
            f"{sum(1 for s in stats if s['truncated'])} truncated (> 5 ms cut); |shift| median {shifts[len(shifts) // 2]:.1f} st, max {shifts[-1]:.1f} st")
    else:
        log("  all fitted words were cached")

    # 4. assemble
    t4 = time.time()
    total_bytes = 0
    lyrics_lines = []
    for p in products:
        buf = array("h", bytes(p.bars * BAR * 2))
        for start, b, slot, w, n, budget in p.placements():
            a = read_wav(cache.fit_path(p.voice, p.plan["pbas"], w, p.shifts.get(w, 0.0), budget))
            end = min(len(a), p.bars * BAR - start)
            buf[start:start + end] = a[:end]
        gain = 10 ** (PEAK_DB / 20) / peak_of(buf)
        for i in range(len(buf)):
            buf[i] = max(-32768, min(32767, int(buf[i] * gain)))
        write_wav(p.path, buf)
        total_bytes += os.path.getsize(p.path)
        lyrics_lines.append(f"{os.path.relpath(p.path, vocal_root)}  [{p.voice}"
                            + (f", root {note_name(p.plan['target'])}, pbas {p.plan['pbas']}" if p.plan["target"] else "") + f", {p.note}]")
        for ln in p.lines:
            lyrics_lines.append("    " + ln.text())
    with open(os.path.join(args.cache, "lyrics.txt"), "w") as f:
        f.write("\n".join(lyrics_lines) + "\n")
    log(f"  assembled {len(products)} files, {total_bytes / 1e6:.1f} MB in {time.time() - t4:.1f}s; lyric sheet: {os.path.join(args.cache, 'lyrics.txt')}")

    # 5. verify
    fails = verify(products, log, jobs=args.jobs, rng_seed=args.seed)
    by = {}
    for p in products:
        by[(p.vclass, p.kind)] = by.get((p.vclass, p.kind), 0) + 1
    log("\nsummary: " + ", ".join(f"{c}/{k} {n}" for (c, k), n in sorted(by.items())))
    folders = sorted({os.path.dirname(p.path) for p in products})
    log(f"{len(products)} files / {total_bytes / 1e6:.1f} MB into {len(folders)} folders; {time.time() - t0:.0f}s total; {fails} verification failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
