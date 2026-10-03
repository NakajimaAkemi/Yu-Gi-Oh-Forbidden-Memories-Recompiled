"""The retail game's tables, read out of SLUS_014.11 and WA_MRG.MRG.

Layouts (notes/card-catalog.md, notes/gameplay-tables.md,
notes/research/fusion-and-drop-tables/, src/game/duel_card_checks.c):

SLUS_014.11 (file offset = RAM address - 0x8000F800)
  0x801D4244 + (id-1)*4   u32 stats: ATK/10 bits 0-8, DEF/10 bits 9-17,
                          guardian star 2 bits 18-21, star 1 bits 22-25,
                          type bits 26-30
  0x801D5332 + id         u8: level in the low nibble, attribute in the high
  names and card texts    the text banks tools/pc/text_listing.py reads

WA_MRG.MRG
  0xB63000 + k*0x75800    the duel package, one copy per terrain (k = 0..6):
    +0x22000  equips      {u16 equip, u16 n, u16 monster[n]}..., 0
    +0x24800  fusions     u16 offset[723], then per card a count (0: 511 -
                          next byte) and 5-byte groups of two 10-bit pairs
    +0x34800  rituals     {u16 ritual, tribute[3], result}..., 0
  0xE99800 + 0x1800*d     opponent d's pools: u16[722] each at +0 (deck),
                          +0x5B4 (S/A-POW), +0xB68 (B/C/D), +0x111C (S/A-TEC)
"""
from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field, replace as dc_replace
from pathlib import Path

try:
    import text_listing as _tl
except ImportError:   # run from elsewhere: text_listing.py sits beside the package
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    import text_listing as _tl

CARD_COUNT = 722
CARD_ID_LIMIT = 32766
DUELIST_COUNT = 40
POOL_TOTAL = 2048
DECK_POOL_MIN_CARDS = 14            # 40 cards at most 3 of each
DECK_SIZE = 40                      # the cards in a deck (save_data.h player_deck)
DECK_COPY_LIMIT = 3                 # copies of one card Build Deck takes back
STARTER_WEIGHT_LIMIT = 32767        # a starter deck's "weight" (src/pc/cards/starter.c)
EXODIA_FIRST_CARD_ID = 0x11         # the five pieces, at ids 17-21 (card_constants.h)
EXODIA_PIECE_COUNT = 5


def exodia_piece(cid: int) -> bool:
    """One of the five pieces, which Build Deck takes only one of
    (build_deck_pane_input.c). A copy a mod adds is another card, so only the
    disc's own ids count."""
    return EXODIA_FIRST_CARD_ID <= cid < EXODIA_FIRST_CARD_ID + EXODIA_PIECE_COUNT


TYPE_NAMES = ["Dragon", "Spellcaster", "Zombie", "Warrior", "Beast-Warrior", "Beast", "Winged Beast", "Fiend",
              "Fairy", "Insect", "Dinosaur", "Reptile", "Fish", "Sea Serpent", "Machine", "Thunder", "Aqua",
              "Pyro", "Rock", "Plant", "Magic", "Trap", "Ritual", "Equip"]
TYPE_MAGIC, TYPE_TRAP, TYPE_RITUAL, TYPE_EQUIP = 20, 21, 22, 23
# The secondary fusion groups a ritual tribute may ask for (cards.h
# CARD_FUSION_GROUP_*, in that order).
FUSION_GROUPS = ("AngelWinged", "Bugrothian", "Egg", "Elf", "FeatherFromBear", "FeatherFromHarpie",
                 "FeatherFromMachine", "Female", "Jar", "Koumorian", "MercuryMagicUser", "MercurySpellcaster",
                 "Mirror", "MusKingian", "MystElfian", "Rainbow", "Sheepian", "Thronian", "Turtle", "UsableBeast",
                 "Horus")
RITUAL_REQUIREMENT_KEYS = ("card", "type", "fusion_group", "min_attack", "min_defense", "max_attack", "max_defense",
                           "min_level", "max_level", "defense_gt_attack")


def fusion_group_named(text) -> str:
    """The group's own spelling, found as the game finds it (letters only,
    any case), or "" for none."""
    if not isinstance(text, str):
        return ""
    key = "".join(c for c in text.lower() if c.isalnum())
    return next((g for g in FUSION_GROUPS if g.lower() == key), "")
# A card's "frame" (cards.c frame_names): the palette rows the game draws a
# card through. Retail picks one by type; purple and orange it never uses.
FRAME_NAMES = ["Monster", "Magic", "Trap", "Ritual", "Purple", "Orange"]


def type_frame(t: int) -> int:
    """The frame the game gives type `t`: equip draws as magic."""
    return {TYPE_MAGIC: 1, TYPE_EQUIP: 1, TYPE_TRAP: 2, TYPE_RITUAL: 3}.get(t, 0)
ATTRIBUTE_NAMES = ["Light", "Dark", "Earth", "Water", "Fire", "Wind"]
STAR_NAMES = ["", "Mars", "Jupiter", "Saturn", "Uranus", "Pluto", "Neptune", "Mercury", "Sun", "Moon", "Venus"]
DUELIST_NAMES = [
    "Unused", "Simon Muran", "Teana", "Jono", "Villager 1", "Villager 2", "Villager 3", "Seto", "Heishin",
    "Rex Raptor", "Weevil Underwood", "Mai Valentine", "Bandit Keith", "Shadi", "Yami Bakura", "Pegasus",
    "Isis", "Kaiba", "Mage Soldier", "Jono 2nd", "Teana 2nd", "Ocean Mage", "High Mage Secmeton",
    "Forest Mage", "High Mage Anubisius", "Mountain Mage", "High Mage Atenza", "Desert Mage",
    "High Mage Martis", "Meadow Mage", "High Mage Kepura", "Labyrinth Mage", "Seto 2nd", "Guardian Sebek",
    "Guardian Neku", "Heishin 2nd", "Seto 3rd", "DarkNite", "Nitemare", "Duel Master K"]
POOLS = ("deck", "pow", "bcd", "tec")
POOL_LABELS = {"deck": "Deck", "pow": "S/A-POW drops", "bcd": "B/C/D drops", "tec": "S/A-TEC drops"}

# --- SLUS ---------------------------------------------------------------
EXE_DELTA = 0x8000F800
STATS_ADDRESS = 0x801D4244
LEVEL_ATTR_ADDRESS = 0x801D5332
NAME_TABLE = _tl.NAME_TABLE             # u16 offsets from 0x801D0000, card n at +2n
NAME_BANK = 0x801D0000
STRING_TABLE = _tl.STRING_TABLE         # card n's text: entry 0x100 + n, offsets from 0x801C0000
DESCRIPTION_BANK = 0x801C0000

# --- WA_MRG -------------------------------------------------------------
TERRAIN_BASE = 0xB63000
TERRAIN_STRIDE = 0x75800
TERRAIN_COPIES = 7
EQUIP_OFFSET, EQUIP_LENGTH = 0x22000, 0x2800
FUSION_OFFSET, FUSION_LENGTH = 0x24800, 0x10000
RITUAL_OFFSET, RITUAL_LENGTH = 0x34800, 0x800
DUELIST_BASE = 0xE99800
DUELIST_STRIDE = 0x1800
POOL_OFFSETS = {"deck": 0x000, "pow": 0x5B4, "bcd": 0xB68, "tec": 0x111C}
STARTER_BASE = 0xF92BD4             # the seven starter deck pools the name entry deals from
STARTER_LENGTH = 7 * (2 + 2 * CARD_COUNT)
# The Password screen's table (src/pc/cards/passwords.c): a record per card
# id from 0, the price in starchips and the password, a BCD nibble per digit,
# both little-endian words. PASSWORD_NONE is a card the screen cannot give.
PASSWORD_TABLE = 0xFB9800
PASSWORD_NONE = 0xFFFFFFFE
# Program images the game loads at 0x80168000 (sector, sectors, what): the
# port runs its own code for them, over their data in guest memory, so a
# mod's code or data there cannot work (src/pc/guest/modules.c).
PROGRAMS = [(7898, 5, "the Free Duel program"), (7968, 15, "the name entry's program"),
            (8054, 15, "the password program"), (8153, 16, "the overworld program"),
            (8311, 16, "the overworld program (after the coup)")]


def slus_offset(address: int) -> int:
    return address - EXE_DELTA


@dataclass
class Card:
    id: int
    name: str = ""
    description: str = ""
    attack: int = 0
    defense: int = 0
    type: int = 0
    attribute: int = 0
    level: int = 0
    star1: int = 0
    star2: int = 0
    frame: int = -1         # FRAME_NAMES index, or -1 for its type's

    FIELDS = ("name", "description", "attack", "defense", "type", "attribute", "level", "star1", "star2", "frame")

    def copy(self, **changes) -> "Card":
        return dc_replace(self, **changes)

    def is_monster(self) -> bool:
        return self.type < TYPE_MAGIC

    def shown_frame(self) -> int:
        """The frame the game draws it in."""
        return self.frame if self.frame >= 0 else type_frame(self.type)

    def same(self, other: "Card") -> bool:
        return all(getattr(self, f) == getattr(other, f) for f in self.FIELDS)


@dataclass
class GameData:
    """Everything the editor shows, as the retail files have it."""
    cards: dict = field(default_factory=dict)          # id -> Card, 1..722
    fusions: dict = field(default_factory=dict)        # (low, high) -> result
    glitch_fusions: set = field(default_factory=set)   # pairs only the game's over-read makes
    equips: dict = field(default_factory=dict)         # equip id -> [monster ids]
    rituals: dict = field(default_factory=dict)        # ritual id -> (t1, t2, t3, result)
    pools: list = field(default_factory=list)          # [duelist][pool] -> {card id: weight}
    passwords: dict = field(default_factory=dict)      # id -> the Password screen's 8 digits, "" for none
    starchips: dict = field(default_factory=dict)      # id -> the disc's Password shop price
    notes: list = field(default_factory=list)          # oddities found while reading
    campaign_map: object = None                        # campaign_map.MapData, None without the overworld packages


# --- reading the executable ----------------------------------------------

def _image(slus: bytes):
    image = _tl.Image.__new__(_tl.Image)
    image.data = slus
    return image


def decode_text(image, address: int, glyphs: dict, limit: int = 1024) -> str:
    """A card name or text as plain UTF-8: 0xFE is a line break, 0xFF the
    end; any control code is written the way the text listing spells it."""
    out = []
    at = address
    end = address + limit
    while at < end:
        code = image.bytes(at, 1)
        if not code:
            break
        code = code[0]
        if code == 0xFF:
            break
        if code == 0xFE:
            out.append("\n")
            at += 1
        elif code < 0xF0:
            out.append(glyphs.get(code, "{g %X}" % code))
            at += 1
        elif code <= 0xF5:
            out.append("{g %X}" % (((code - 0xF0) << 8) | image.bytes(at + 1, 1)[0]))
            at += 2
        elif code == 0xF8 and image.bytes(at + 1, 1) and image.bytes(at + 1, 1)[0] in (0x0A, 0x0B):
            out.append("{f8 %02X %02X}" % tuple(image.bytes(at + 1, 2)))      # a colour or an icon, as the listing
            at += 3
        else:
            out.append("{%02X}" % code)
            at += 1
    return "".join(out)


def plain_names(image, glyphs: dict) -> dict:
    """Card names as text: a colour or icon code (F8 0A NN, F8 0B NN) at
    the start or inside, as community mods write them, is skipped."""
    names = {}
    for cid in range(1, CARD_COUNT + 1):
        at = NAME_BANK + image.u16(NAME_TABLE + cid * 2)
        text = ""
        while len(text) < 64:
            code = image.bytes(at, 1)[0]
            if code == 0xF8:
                at += 3 if image.bytes(at + 1, 1)[0] in (0x0A, 0x0B) else 2
                continue
            if code >= 0xF0:
                break
            text += glyphs.get(code, "?")
            at += 1
        names[cid] = text
    return names


def text_bytes(image, address: int, limit: int = 1024) -> bytes:
    """A string's bytes, through its 0xFF."""
    data = image.bytes(address, limit)
    end = data.find(b"\xFF")
    return data[:end + 1] if end >= 0 else data


# Card texts kept past the retail end of WA_MRG.MRG, 256 bytes a card (the
# patch kit's mods): the executable's string table then points descriptions
# 1-8 at eight slots of 0x100 the mod loads a sector into.
WA_TEXT_BASE, WA_TEXT_SLOT = 0x2400000, 0x100


def wa_descriptions(slus: bytes, wa: bytes) -> bool:
    image = _image(slus)
    return len(wa) >= WA_TEXT_BASE + CARD_COUNT * WA_TEXT_SLOT and \
        all(image.u16(STRING_TABLE + (0x101 + k) * 2) == 0xA00 + 0x100 * k for k in range(8))


def description_bytes(slus: bytes, wa: bytes = b"") -> dict:
    """{card id: its description's bytes}, from the executable's bank or,
    when the mod keeps them there, from WA_MRG.MRG."""
    if wa and wa_descriptions(slus, wa):
        out = {}
        for cid in range(1, CARD_COUNT + 1):
            slot = wa[WA_TEXT_BASE + (cid - 1) * WA_TEXT_SLOT:WA_TEXT_BASE + cid * WA_TEXT_SLOT]
            end = slot.find(b"\xFF")
            out[cid] = slot[:end + 1] if end >= 0 else slot + b"\xFF"
        return out
    image = _image(slus)
    return {cid: text_bytes(image, DESCRIPTION_BANK + image.u16(STRING_TABLE + (0x100 + cid) * 2))
            for cid in range(1, CARD_COUNT + 1)}


class _Bytes:
    """A string's bytes where decode_text looks for them."""

    def __init__(self, data: bytes):
        self.data = data

    def bytes(self, address: int, count: int) -> bytes:
        return self.data[address:address + count]


def read_cards(slus: bytes, wa: bytes = b"") -> dict:
    image = _image(slus)
    glyphs = _tl.glyph_characters(image)
    names = plain_names(image, glyphs)
    texts = description_bytes(slus, wa)
    cards = {}
    for cid in range(1, CARD_COUNT + 1):
        stats = image.u32(STATS_ADDRESS + (cid - 1) * 4)
        level_attr = image.bytes(LEVEL_ATTR_ADDRESS + cid, 1)[0]
        cards[cid] = Card(
            id=cid,
            name=names.get(cid, ""),
            description=decode_text(_Bytes(texts[cid]), 0, glyphs, len(texts[cid])),
            attack=(stats & 0x1FF) * 10,
            defense=((stats >> 9) & 0x1FF) * 10,
            star2=(stats >> 18) & 0xF,
            star1=(stats >> 22) & 0xF,
            type=(stats >> 26) & 0x1F,
            level=level_attr & 0xF,
            attribute=level_attr >> 4)
    return cards


def pack_stats(card: Card) -> int:
    return ((card.attack // 10) & 0x1FF) | (((card.defense // 10) & 0x1FF) << 9) | ((card.star2 & 0xF) << 18) | \
           ((card.star1 & 0xF) << 22) | ((card.type & 0x1F) << 26)


# --- reading the archive ---------------------------------------------------

def decode_fusions(table: bytes):
    """(pairs, glitch pairs) as Duel_CheckFusion reads the table: a pair is
    kept under the smaller id and the first match wins. An odd count's last
    group has three bytes, but the game still compares the second pair, made
    of the next two bytes: those are the "glitch" fusions."""
    pairs, glitch = {}, set()
    for a in range(CARD_COUNT + 1):
        off = struct.unpack_from("<H", table, a * 2)[0]
        if off == 0 or off >= len(table):
            continue
        p = off
        n = table[p]
        if n == 0:
            n = 0x1FF - table[p + 1]
            p += 1
        p += 1
        while n > 0 and p + 5 <= len(table):
            g = table[p]
            for k, (sp, sr) in enumerate(((8, 6), (4, 2))):
                partner = ((g << sp) & 0x300) | table[p + 1 + 2 * k]
                result = ((g << sr) & 0x300) | table[p + 2 + 2 * k]
                if not (1 <= partner <= CARD_COUNT and 1 <= result <= CARD_COUNT):
                    continue
                pair = (min(a, partner), max(a, partner))
                if partner < a or pair in pairs:
                    continue       # never asked (the key is the smaller id), or shadowed
                pairs[pair] = result
                if k == 1 and n == 1:
                    glitch.add(pair)
            p += 5
            n -= 2
    return pairs, glitch


def decode_equips(table: bytes) -> dict:
    equips = {}
    p = 0
    while p + 4 <= len(table):
        key, n = struct.unpack_from("<HH", table, p)
        if key == 0:
            break
        p += 4
        monsters = [struct.unpack_from("<H", table, p + 2 * i)[0] for i in range(n) if p + 2 * i + 2 <= len(table)]
        p += 2 * n
        if key not in equips:          # the first record for an equip is the one read
            equips[key] = [m for m in monsters if m]
    return equips


def decode_rituals(table: bytes) -> dict:
    rituals = {}
    for p in range(0, len(table) - 9, 10):
        record = struct.unpack_from("<5H", table, p)
        if record[0] == 0:
            break
        rituals.setdefault(record[0], tuple(record[1:]))
    return rituals


def decode_pool(block: bytes, offset: int) -> dict:
    weights = struct.unpack_from("<%dH" % CARD_COUNT, block, offset)
    return {cid: w for cid, w in enumerate(weights, 1) if w}


def terrain_block(wa: bytes, copy: int = 0) -> bytes:
    start = TERRAIN_BASE + copy * TERRAIN_STRIDE
    return wa[start:start + TERRAIN_STRIDE]


def read_archive(wa: bytes, data: GameData):
    block = terrain_block(wa)
    if len(block) < RITUAL_OFFSET + RITUAL_LENGTH:
        raise ValueError("WA_MRG.MRG is too short to be the retail archive")
    data.fusions, data.glitch_fusions = decode_fusions(block[FUSION_OFFSET:FUSION_OFFSET + FUSION_LENGTH])
    data.equips = decode_equips(block[EQUIP_OFFSET:EQUIP_OFFSET + EQUIP_LENGTH])
    data.rituals = decode_rituals(block[RITUAL_OFFSET:RITUAL_OFFSET + RITUAL_LENGTH])
    for k in range(1, TERRAIN_COPIES):
        other = terrain_block(wa, k)
        if other != block:
            for name, off, length in (("equip", EQUIP_OFFSET, EQUIP_LENGTH), ("fusion", FUSION_OFFSET, FUSION_LENGTH),
                                      ("ritual", RITUAL_OFFSET, RITUAL_LENGTH)):
                if other[off:off + length] != block[off:off + length]:
                    data.notes.append(f"terrain copy {k}'s {name} table differs from copy 0; copy 0 is the one shown")
    data.pools = []
    for d in range(DUELIST_COUNT):
        record = wa[DUELIST_BASE + d * DUELIST_STRIDE:DUELIST_BASE + (d + 1) * DUELIST_STRIDE]
        data.pools.append({pool: decode_pool(record, off) for pool, off in POOL_OFFSETS.items()})


def _bcd(value: int) -> bool:
    return all(((value >> (4 * i)) & 0xF) <= 9 for i in range(8))


def read_passwords(wa: bytes) -> dict:
    """{card id: its password as 8 digits, or "" for none}, as the port reads
    the Password screen's table; {} when the archive stops short of it."""
    if len(wa) < PASSWORD_TABLE + 8 * (CARD_COUNT + 1):
        return {}
    out = {}
    for cid in range(1, CARD_COUNT + 1):
        value = struct.unpack_from("<I", wa, PASSWORD_TABLE + 8 * cid + 4)[0]
        out[cid] = f"{value:08x}" if value != PASSWORD_NONE and _bcd(value) else ""
    return out


def read_game(slus: bytes, wa: bytes) -> GameData:
    data = GameData(cards=read_cards(slus, wa))
    read_archive(wa, data)
    data.passwords = read_passwords(wa)
    if data.passwords:
        data.starchips = {cid: struct.unpack_from("<I", wa, PASSWORD_TABLE + 8 * cid)[0]
                          for cid in data.passwords}
    from . import campaign_map
    data.campaign_map = campaign_map.read(slus, wa)
    if data.campaign_map is not None:
        data.notes.extend(data.campaign_map.notes)
    return data


def load_game(files) -> GameData:
    """GameData from disc.GameFiles."""
    return read_game(files.slus, files.wa)


# --- encoders (the test fixtures, and checks) --------------------------------

def encode_fusions(pairs: dict) -> bytes:
    """A fusion table laid out as the disc's: pairs under the smaller id."""
    by_card = {}
    for (low, high), result in sorted(pairs.items()):
        by_card.setdefault(low, []).append((high, result))
    table = bytearray(FUSION_LENGTH)
    p = (CARD_COUNT + 1) * 2
    for a in range(CARD_COUNT + 1):
        entries = by_card.get(a)
        if not entries:
            continue
        struct.pack_into("<H", table, a * 2, p)
        n = len(entries)
        if n < 256:
            table[p] = n
            p += 1
        else:
            table[p] = 0
            table[p + 1] = 0x1FF - n
            p += 2
        for i in range(0, n, 2):
            group = entries[i:i + 2] + [(0, 0)] * (2 - len(entries[i:i + 2]))
            (b1, r1), (b2, r2) = group
            # the reader shifts the control byte left by 8, 6, 4, 2: bits 0-1 are the first partner's high bits
            table[p] = ((b1 >> 8) & 3) | (((r1 >> 8) & 3) << 2) | (((b2 >> 8) & 3) << 4) | (((r2 >> 8) & 3) << 6)
            table[p + 1] = b1 & 0xFF
            table[p + 2] = r1 & 0xFF
            if len(entries[i:i + 2]) == 2:
                table[p + 3] = b2 & 0xFF
                table[p + 4] = r2 & 0xFF
                p += 5
            else:
                p += 3
    return bytes(table)


def encode_equips(equips: dict) -> bytes:
    out = bytearray()
    for equip, monsters in equips.items():
        out += struct.pack("<HH", equip, len(monsters))
        out += struct.pack("<%dH" % len(monsters), *monsters)
    out += b"\0\0"
    return bytes(out.ljust(EQUIP_LENGTH, b"\0"))


def encode_rituals(rituals: dict) -> bytes:
    out = bytearray()
    for ritual, recipe in rituals.items():
        out += struct.pack("<5H", ritual, *recipe)
    return bytes(out.ljust(RITUAL_LENGTH, b"\0"))


def encode_pool(pool: dict) -> bytes:
    return struct.pack("<%dH" % CARD_COUNT, *(pool.get(cid, 0) for cid in range(1, CARD_COUNT + 1)))
