/**
 * Jackpot — browser demo.
 *
 * A rigged casino as two FFGL plugins: SW Jackpot (JP01, a source: a one-armed
 * bandit, a roulette wheel, the money wheel, a craps table or the lottery
 * drum, with chips and coins showering on a win) and SW Jackpot Over (JP02, an
 * effect: the same over the clip, which can also be a reel symbol). The one
 * idea, from AGENTS.md: every casino game has a symmetry the physics cannot
 * see — a roulette rotor's frets are identical, so are the money wheel's pegs,
 * a die is face-transitive, lottery balls are alike — so each game is
 * simulated honestly, ahead of time, and then the PAINT is turned so the
 * result shown is the one drawn. The slot machine needs no symmetry: the
 * random number at the pull is the result, and the reels are driven to it.
 *
 * ---------------------------------------------------------------------------
 * What runs here, and what does not
 * ---------------------------------------------------------------------------
 *
 * **This page does not port the plugin. It runs it.** `jackpot-core.wasm` is
 * the plugin's own C++ compiled UNMODIFIED with emscripten
 * (demo/tools/build-wasm.sh): `source/Jackpot.cpp` — the plugin class, with
 * its constructor, its clock and seconds-or-milliseconds vote, Play and Auto
 * Play, the requests it makes, the wheel file, font resolution, the uploads
 * and every uniform it sets — and everything it calls: the five planners and
 * their physics (Slots, Roulette, MoneyWheel, Craps on polyhedral's dice
 * engine, Lottery), the shower and its pile, WheelFace, FileBytes, SymbolArt,
 * Typeface, Controls, Common, Shaders and Diag, with the FFGL SDK's
 * CFFGLPlugin, its parameter bookkeeping, FFGLShader and FFGLScreenQuad. Its
 * GL calls go to this page's WebGL2 context through emscripten's GL library.
 *
 * What is the page's and not the plugin's:
 *
 *   - **The host.** demo/wasm/glue.cpp constructs the plugin (what the two
 *     registration files do in a bundle), reads its parameter declarations
 *     back through the SDK's host getters — the panel below is built from
 *     them, not from a list typed here — and forwards SetFloatParameter,
 *     SetTextParameter, SetTime and ProcessOpenGL, as tools/jptest's Rig does.
 *   - **Four GL entry points** (demo/wasm/gl_shim.cpp). glShaderSource: the
 *     plugin hands GL desktop GLSL 4.10; `shaderSource()` below refuses any
 *     text that is not byte-for-byte one of this page's copies of the
 *     plugin's programs (shaders.js, held to source/Shaders.cpp and its
 *     Programs() list by demo/tools/check_shaders.py), then applies the kit's
 *     `port()` — the version line and the ES precision defaults, nothing
 *     else. glEnable / glDisable / glIsEnabled: GL_PROGRAM_POINT_SIZE, which
 *     the plugin saves and restores every frame, does not exist in WebGL2.
 *   - **Planning on the page's thread.** The plugin plans roulette, the money
 *     wheel, craps and the lottery with std::async; this module has no
 *     threads, so glue.cpp calls the plugin's own SetSynchronousForTest(true),
 *     as its harness does, and the frame after a press waits for the planner.
 *   - **The panel's read-outs.** The number beside a slider is Controls.cpp's
 *     conversion, compiled in (glue.cpp's jp_convert pairs each control with
 *     the function Jackpot.cpp applies to it); the units printed after it, and
 *     every tooltip, are this page's words.
 *   - **Files.** Wheel File and Font File are paths in the plugin; here a file
 *     you pick is written into emscripten's in-memory file system and the path
 *     handed to the plugin, which reads it with its own loaders.
 *
 * ---------------------------------------------------------------------------
 * Decisions this page made, and why
 * ---------------------------------------------------------------------------
 *
 * **Both plugins, one page**, as polyhedral's demo does it: the Plugin
 * dropdown is a new instance at that constructor's defaults. The effect
 * declares one more Symbols element (Clip) and Mix; Symbols is two rows of the
 * same name and only the chosen plugin's is shown. `?plugin=over` opens the
 * effect.
 *
 * **Kit types for FFGL types.** The kit has no event, integer or file control.
 * Play and Shower Now (FF_TYPE_EVENT) are buttons that send the rising edge
 * and the release, 1 then 0, as the harness's Press() does. Fixed Number,
 * Seed, Segments, Balls and Draw (FF_TYPE_INTEGER) are number fields clamped
 * to the declared range. The two file parameters show their path with a
 * picker beside it. Font Name is sent when the field is committed (Enter, or
 * leaving it), as a host sends text.
 *
 * **An example wheel file.** Wheel File is the 0.2.0 feature and a visitor
 * rarely has one to hand, so beside the picker is the README's own example,
 * tools/fixtures/showcase.txt, served as demo/showcase.txt and held to the
 * fixture byte for byte by check_shaders.py. Choosing it also sets Game to
 * Money Wheel and Segments to 12, the README's picture, and the button says so.
 *
 * **The About block is not on the panel**, as on every page in the suite.
 */

import { mountDemo } from './vendor/demo.js';
import { port, GLError } from './vendor/gl.js';
import * as S from './shaders.js';
import createJackpot from './jackpot-core.js';

//===========================================================================
// FFGL.h's parameter types, by number: what GetParamType returns.
//===========================================================================

const FF_TYPE_BOOLEAN = 0;
const FF_TYPE_EVENT = 1;
const FF_TYPE_RED = 2;
const FF_TYPE_GREEN = 3;
const FF_TYPE_BLUE = 4;
const FF_TYPE_STANDARD = 10;
const FF_TYPE_OPTION = 11;
const FF_TYPE_INTEGER = 13;
const FF_TYPE_FILE = 14;
const FF_TYPE_TEXT = 100;

//===========================================================================
// The GLSL: the page's checked copy, assembled as shaders::Programs()
// assembles the plugin's (every stage the pieces it joins, in order).
//===========================================================================

const PAGE_PROGRAMS = S.PROGRAMS.map((p) => ({
  name: p.name,
  vertex: p.vertex.map((piece) => S[piece]).join(''),
  fragment: p.fragment.map((piece) => S[piece]).join(''),
}));

/** Which programs' stages the compiled plugin handed over, and any refused. */
const shaderCheck = { seen: new Set(), refused: 0 };

/**
 * Called by gl_shim.cpp's glShaderSource with the text the compiled plugin
 * handed to GL. Anything but one of this page's own copies is refused (an
 * empty source, so the plugin's InitGL fails and says so) rather than compiled.
 */
function shaderSource(stage, text) {
  const match = PAGE_PROGRAMS.find((p) => p[stage] === text);
  if (!match) {
    shaderCheck.refused += 1;
    return '';
  }
  shaderCheck.seen.add(`${match.name}.${stage}`);
  return port(text, { stage });
}

//===========================================================================
// The module.
//===========================================================================

const printed = [];
let core;
try {
  core = await createJackpot({
    jackpotShaderSource: shaderSource,
    print: (text) => { printed.push(text); console.log(text); },
  });
} catch (error) {
  const root = document.querySelector('#demo');
  root.textContent = `The demo could not start: this browser would not run its WebAssembly (${error.message}). The plugin, its source and its downloads are at https://github.com/stoatworks-labs/jackpot.`;
  throw error;
}

const text = (pointer) => core.UTF8ToString(pointer);

/** A C string for one call: allocated, handed over, freed. */
function withCString(value, fn) {
  const pointer = core.stringToNewUTF8(value);
  try {
    return fn(pointer);
  } finally {
    core._free(pointer);
  }
}

//===========================================================================
// The declarations, read from each plugin's constructor through the SDK.
//===========================================================================

function declarations(effect) {
  const h = core._jp_new(effect ? 1 : 0);
  const list = [];
  const n = core._jp_param_count(h);
  for (let i = 0; i < n; i += 1) {
    const type = core._jp_param_type(h, i);
    const d = { index: i, name: text(core._jp_param_name(h, i)), type, group: text(core._jp_param_group(h, i)) };
    if (type === FF_TYPE_TEXT || type === FF_TYPE_FILE) d.default = text(core._jp_param_default_text(h, i));
    else d.default = core._jp_param_default(h, i);
    if (type === FF_TYPE_OPTION) {
      d.elements = [];
      d.values = [];
      for (let e = 0; e < core._jp_param_element_count(h, i); e += 1) {
        d.elements.push(text(core._jp_param_element_name(h, i, e)));
        d.values.push(core._jp_param_element_value(h, i, e));
      }
    }
    if (type === FF_TYPE_INTEGER) {
      d.min = core._jp_param_range_min(h, i);
      d.max = core._jp_param_range_max(h, i);
    }
    if (type === FF_TYPE_FILE) {
      d.extensions = [];
      for (let e = 0; e < core._jp_param_extension_count(h, i); e += 1) d.extensions.push(text(core._jp_param_extension(h, i, e)));
    }
    list.push(d);
  }
  const maxInputs = core._jp_max_inputs(h);
  core._jp_delete(h);
  return { list, maxInputs };
}

const DECLARED = { source: declarations(false), over: declarations(true) };

//===========================================================================
// The read-outs: Controls.cpp's number, this page's unit.
//===========================================================================

const fixed = (n, d) => n.toFixed(d);
const UNITS = {
  'Spin Time': (x) => `${fixed(x, 2)} s`,
  Interval: (x) => `${fixed(x, x < 10 ? 1 : 0)} s`,
  Blur: (x) => `${fixed(x, 2)} frame`,
  'Reel Bounce': (x) => `ζ ${fixed(x, 2)}`,
  'Rotor Speed': (x) => `${fixed(x, 2)} rad/s`,
  Clapper: (x) => `${fixed(x, 1)} N·m/rad`,
  Air: (x) => `${fixed(x, 2)} g`,
  Amount: (x) => `${fixed(x, 0)} /s`,
  'Piece Size': (x) => `${fixed(100 * x, 1)}% h`,
  'Light Angle': (x) => `${fixed(x, 0)}°`,
  Tilt: (x) => `${fixed(x, 0)}°`,
  Zoom: (x) => `× ${fixed(x, 2)}`,
};

function readout(d) {
  return (v) => {
    const x = core._jp_convert(d.index, Math.fround(v));
    if (Number.isNaN(x)) return fixed(v, 2);
    return (UNITS[d.name] ?? ((y) => fixed(y, 3)))(x);
  };
}

/** Tooltips: this page's words, from the README's description of each control. */
const HINTS = {
  Game: 'Slots, Roulette, Money Wheel, Craps, Lottery, or Shower Only (the chips and coins on their own). Changing it lays the new game out at rest.',
  Play: 'Play the game: it runs with real physics and comes to rest on the result at exactly Spin Time (or the beat or bar Land On picks). FF_TYPE_EVENT: a press, then a release.',
  'Spin Time': 'Seconds from Play to rest, 1 to 20, geometric. A game is simulated at its own pace and played faster or slower to land on it; craps holds the throw rather than throwing harder, and the lottery plays fast below about 6 s.',
  'Land On': 'Time: at Spin Time. Beat or Bar: on the first beat or bar boundary at least Spin Time away. There is no host transport on this page, so that is the plugin’s own 120 BPM clock.',
  Result: 'Random (weighted as the real game is), Win, Jackpot, Near Miss, Lose, or Fixed: Fixed Number. Read against the bet, which is Fixed Number.',
  'Fixed Number': 'The bet, and what Fixed shows: the slot symbol (0 blank … 10 seven, 11 diamond), the roulette number (37 is 00), the money wheel’s value (on a wheel file, its line), the craps total, the lottery ball. FF_TYPE_INTEGER, 0–99.',
  Seed: 'The same Seed plays the same sequence of plays. FF_TYPE_INTEGER, 0–9999.',
  'Auto Play': 'Play every Interval. Switching it on plays now.',
  Interval: 'Seconds between automatic plays, 2 to 120, geometric.',
  Reels: 'Three or five reels.',
  Symbols: 'Fruit, or Sevens and Bars; on SW Jackpot Over, Clip too — the clip where the seven would be.',
  Strip: 'Virtual: each reel drawn on 64 virtual stops over 22 physical ones, so the seven sails past the line far more often than it lands (as real machines are built). Physical: a fair strip.',
  Blur: 'The shutter: each reel’s strip integrated over this much of a frame, so the symbols smear at speed and sharpen as the reel brakes.',
  'Reel Bounce': 'How lively the detent is when a reel stops: the damping ratio ζ, 1 (dead) to 0.18.',
  Wheel: 'European (37 pockets) or American (38, with 00).',
  'Rotor Speed': 'The rotor’s speed at the start, 0 to 4 rad/s; it spins down over about 40 s and turns on between plays.',
  Deflectors: 'The diamonds on the stator, which the ball may hit on its way down.',
  Clapper: 'The leather clapper’s stiffness, 2 to 40 N·m/rad: every peg it meets takes energy, which is what slows the wheel.',
  Segments: 'How many segments the money wheel has, 2–54. With no Wheel File, a smaller Big Six. FF_TYPE_INTEGER.',
  'Wheel File': 'A .txt, one value a line: * marks the jackpot, % the joker, and 🍒 🍋 🍊 🔔 🍉 💎 7️⃣ alone on a line paint the reel art. The plugin takes a path; here a file you pick is written into the page’s memory and read by the plugin’s own loader. Nothing is uploaded.',
  Lines: 'With more lines than Segments: In Order takes the first ones, Random shuffles every line by Seed. The draw changes only when the file, Seed, Segments, Lines or Short File does.',
  'Short File': 'With fewer lines than Segments: Repeat them round the wheel, or make the wheel Fewer Segments (two at least).',
  Pyramids: 'The rubber pyramids on the back wall the dice bounce off.',
  Puck: 'The ON/OFF puck that marks the point.',
  Balls: 'How many balls in the drum, 10–75. FF_TYPE_INTEGER.',
  Draw: 'How many are drawn, 1–7. FF_TYPE_INTEGER.',
  Air: 'The blower’s lift at the jet’s core, 0.5 to 4 times g.',
  Shower: 'When the chips and coins come down: Off, On Win, On Jackpot, Always.',
  'Shower Now': 'A shower now, whatever the game did. FF_TYPE_EVENT.',
  Pattern: 'Rain, Fountain, Burst or Pour.',
  Pieces: 'Chips, Coins, or Mixed.',
  Amount: 'Pieces thrown a second, 0 to 400, quadratic.',
  'Piece Size': 'A chip’s diameter as a share of the frame’s height h, 2% to 20%, geometric.',
  Pile: 'Let the pieces settle into a pile along the foot of the frame; each fades 12 s after it was thrown.',
  Backdrop: 'None: transparent (on SW Jackpot Over, the clip). Felt or Dark.',
  'Felt Colour': 'The felt’s colour: the backdrop, the roulette and craps tables.',
  'Accent Colour': 'The slot machine’s cabinet and the trim.',
  Lights: 'The lamps: the slot machine’s chase, the bulbs round the boards.',
  Display: 'The slot machine’s banner and the other games’ result board, once the play is at rest.',
  Font: 'Built-in, then every font installed on the machine. A web page cannot list this computer’s fonts, and the plugin’s own scan finds none in the page’s memory, so the list here is Built-in alone.',
  'Font File': 'A .ttf, .otf, .ttc or .otc you pick, written into the page’s memory and loaded by the plugin’s own font loader. It wins over Font and Font Name.',
  'Font Name': 'The family, as text, which wins over Font. Typed in, it travels with a composition; a family picked from Font is saved by Arena as its place in the list. With no installed fonts here, any name falls back to the built-in face — the plugin’s own path for a composition opened on a machine without its font. Sent on Enter.',
  'Light Angle': 'Where the key light stands, −180° to 180°. Roulette, craps, the lottery and the shower only.',
  Tilt: 'The camera’s elevation, 20° to straight down; the lottery is seen 35° lower. Roulette, craps, the lottery and the shower only.',
  Zoom: 'Framing, 0.5 (wide) to 2.5 (close), geometric.',
  Mix: 'The effect against the clip. At 0 the clip is returned exactly.',
};

//===========================================================================
// Kit parameters from the declarations. The id is the name as Arena
// addresses it (lower case, spaces removed).
//===========================================================================

const KIT_TYPES = {
  [FF_TYPE_STANDARD]: 'standard',
  [FF_TYPE_OPTION]: 'option',
  [FF_TYPE_BOOLEAN]: 'boolean',
  [FF_TYPE_EVENT]: 'boolean',
  [FF_TYPE_RED]: 'colour',
  [FF_TYPE_GREEN]: 'colour',
  [FF_TYPE_BLUE]: 'colour',
  [FF_TYPE_INTEGER]: 'text',
  [FF_TYPE_FILE]: 'text',
  [FF_TYPE_TEXT]: 'text',
};

/**
 * The kit reads `embed`, `size`, `clip` and `bg` from the query string itself,
 * and this page reads `plugin`; a parameter id equal to one would be set by it.
 */
const RESERVED_QUERY = new Set(['embed', 'size', 'clip', 'bg', 'plugin']);
const addressOf = (name) => {
  const address = name.toLowerCase().replace(/\s+/g, '');
  return RESERVED_QUERY.has(address) ? `param-${address}` : address;
};

function kitDefault(d) {
  switch (d.type) {
    case FF_TYPE_OPTION: {
      const i = d.values.findIndex((v) => v === d.default);
      return i >= 0 ? i : Math.round(d.default);
    }
    case FF_TYPE_INTEGER: return String(Math.round(d.default));
    case FF_TYPE_BOOLEAN:
    case FF_TYPE_EVENT: return d.default > 0.5 ? 1 : 0;
    default: return d.default;
  }
}

function kitParam(d, id, only) {
  const p = { id, name: d.name, type: KIT_TYPES[d.type], group: d.group, hint: HINTS[d.name], ff: d, only };
  if (p.type === undefined) throw new Error(`no kit control for FFGL type ${d.type} (${d.name})`);
  if (d.type === FF_TYPE_OPTION) p.elements = d.elements;
  if (d.type === FF_TYPE_STANDARD) p.display = readout(d);
  return p;
}

const sameDeclaration = (a, b) => JSON.stringify({ ...a, index: 0 }) === JSON.stringify({ ...b, index: 0 });

const PARAMS = [];
for (const d of DECLARED.over.list) {
  if (d.group === 'About') continue;
  const s = DECLARED.source.list.find((x) => x.index === d.index);
  if (s && s.group !== 'About' && sameDeclaration(s, d)) {
    PARAMS.push(kitParam(d, addressOf(d.name)));
  } else {
    if (s) PARAMS.push(kitParam(s, addressOf(s.name), 'source'));
    PARAMS.push(kitParam(d, addressOf(d.name) + (s ? '_over' : ''), 'over'));
  }
}
for (const s of DECLARED.source.list) {
  if (s.group !== 'About' && !DECLARED.over.list.some((d) => d.index === s.index)) PARAMS.push(kitParam(s, addressOf(s.name), 'source'));
}

/** Each constructor's defaults, by kit id. */
const DEFAULTS = { source: {}, over: {} };
for (const p of PARAMS) {
  for (const variant of ['source', 'over']) {
    const d = DECLARED[variant].list.find((x) => x.index === p.ff.index);
    DEFAULTS[variant][p.id] = d && (!p.only || p.only === variant) ? kitDefault(d) : kitDefault(p.ff);
  }
}

const query = new URLSearchParams(window.location.search);
const initialVariant = query.get('plugin') === 'over' ? 'over' : 'source';
for (const p of PARAMS) p.default = DEFAULTS[initialVariant][p.id];

const declaredNamed = (name) => DECLARED.over.list.find((d) => d.name === name);
const GAME_NAMES = declaredNamed('Game')?.elements ?? [];

//===========================================================================
// The renderer: one plugin instance, driven as a host drives it.
//===========================================================================

/** The value the host would send for a kit control, or null for none. */
function hostValue(p, v) {
  const d = p.ff;
  switch (d.type) {
    case FF_TYPE_OPTION: return Math.fround(d.values[Math.round(v)] ?? 0);
    case FF_TYPE_BOOLEAN: return v > 0.5 ? 1 : 0;
    case FF_TYPE_EVENT: return null;
    case FF_TYPE_INTEGER: {
      const n = Number.parseInt(String(v).trim(), 10);
      if (!Number.isFinite(n)) return null;
      return Math.min(d.max, Math.max(d.min, n));
    }
    case FF_TYPE_FILE:
    case FF_TYPE_TEXT: return String(v);
    default: return Math.fround(v);
  }
}

function createRenderer(gl) {
  // emscripten's GL library, pointed at the kit's context. Nothing is turned
  // on behind the kit's back: no extensions are enabled on its behalf.
  const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
  core.GL.makeContextCurrent(handle);
  core._jp_prepare_log();

  let instance = 0;
  let variant = null;
  let pushed = new Map();
  let clipName = 0;
  let clipTexture = null;
  let frames = 0;
  let plays = 0;
  let planFrameMs = null;

  /** A kit texture under a GL name the plugin can bind, in emscripten's tables. */
  function glName(texture) {
    if (texture === clipTexture) return clipName;
    if (clipName) core.GL.textures[clipName] = null;
    clipName = core.GL.getNewId(core.GL.textures);
    core.GL.textures[clipName] = texture;
    texture.name = clipName;
    clipTexture = texture;
    return clipName;
  }

  function start(which, width, height) {
    if (instance) core._jp_delete(instance);
    instance = core._jp_new(which === 'over' ? 1 : 0);
    variant = which;
    pushed = new Map();
    frames = 0;
    plays = 0;
    planFrameMs = null;
    if (!core._jp_init_gl(instance, width, height)) {
      const why = shaderCheck.refused > 0
        ? 'the GLSL the compiled plugin handed to GL is not this page’s checked copy of source/Shaders.cpp, so it was not compiled'
        : `the plugin’s InitGL failed${printed.length ? `: ${printed.slice(-3).join(' ')}` : ''}`;
      throw new GLError(`${which === 'over' ? 'SW Jackpot Over' : 'SW Jackpot'} could not start: ${why}.`);
    }
  }

  function push(params) {
    for (const p of PARAMS) {
      if (p.only && p.only !== variant) continue;
      const value = hostValue(p, params.get(p.id));
      if (value === null || pushed.get(p.id) === value) continue;
      pushed.set(p.id, value);
      if (typeof value === 'string') withCString(value, (s) => core._jp_set_text(instance, p.ff.index, s));
      else core._jp_set_float(instance, p.ff.index, value);
    }
  }

  /** What the plugin changed of its own accord: Font Name after a Font File. */
  function readBack(params) {
    for (const p of PARAMS) {
      if (p.ff.type !== FF_TYPE_TEXT || (p.only && p.only !== variant)) continue;
      const now = text(core._jp_get_text(instance, p.ff.index));
      if (now !== pushed.get(p.id)) {
        pushed.set(p.id, now);
        params.set(p.id, now);
      }
    }
  }

  return {
    render({ input, params, width, height, time, variant: wanted }) {
      core.GL.makeContextCurrent(handle);
      if (!instance || wanted !== variant) start(wanted ?? 'source', width, height);
      push(params);
      const clip = variant === 'over' ? glName(input.texture) : 0;
      const before = performance.now();
      if (!core._jp_process(instance, time, clip, input.width, input.height)) {
        throw new GLError('The plugin’s ProcessOpenGL failed.');
      }
      const spent = performance.now() - before;
      readBack(params);
      // The frame a play started on is the frame its plan was made on: the
      // page's measurement of how long the page's thread waited for it.
      const started = core._jp_plays_started(instance);
      if (started !== plays) {
        plays = started;
        planFrameMs = spent;
      }
      // For anything checking the page from outside: frames drawn by this
      // instance, and the plays it has started (the harness's counter).
      frames += 1;
      gl.canvas.dataset.frames = String(frames);
      gl.canvas.dataset.playsStarted = String(started);
    },
    press(index) {
      if (!instance) return;
      core._jp_set_float(instance, index, 1);
      core._jp_set_float(instance, index, 0);
    },
    /** The play the plugin is playing (CurrentPlay, the harness's accessor). */
    play() {
      if (!instance) return null;
      const game = core._jp_play_game(instance);
      return {
        game,
        plays: core._jp_plays_started(instance),
        finished: core._jp_play_finished(instance) === 1,
        text: text(core._jp_play_text(instance)),
        kind: core._jp_play_kind(instance),
        natural: core._jp_play_natural(instance),
        duration: core._jp_play_duration(instance),
        warp: core._jp_play_warp(instance),
        credits: core._jp_credits(instance),
        point: core._jp_craps_point(instance),
        pieces: core._jp_shower_pieces(instance),
        planFrameMs,
      };
    },
    log() {
      try {
        return core.FS.readFile(text(core._jp_log_path()), { encoding: 'utf8' });
      } catch {
        return '';
      }
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

let renderer = null;

const mounted = mountDemo({
  name: 'Jackpot',
  pluginId: 'JP01 · JP02',
  kind: ['source', 'effect'],
  tagline:
    'A thoroughly rigged casino for Resolume: a one-armed bandit, a roulette wheel, the money wheel, a craps table and the lottery drum (SW Jackpot), or the same over your clip (SW Jackpot Over), with chips and coins raining down on a win. Press Play: the game plays out with real physics and comes to rest on a random result, or the one you picked, at exactly Spin Time. Each game is simulated honestly, then its paint is turned — the numbered ring, the segments, the dice faces, the numbers dealt to the balls — so the result shown is the one drawn.',
  repo: 'https://github.com/stoatworks-labs/jackpot',
  page: 'https://stoatworks-labs.com/software/jackpot/',
  video: 'https://www.youtube.com/watch?v=-s5chJo2Idc',

  blurb:
    'It is the plugin’s own C++ — the plugin class, the five games’ planners and physics, the shower, the wheel files and the fonts, with the FFGL SDK classes it uses — compiled unmodified to WebAssembly, drawing with its own GLSL in WebGL2. The host is this page: plays are planned on the page’s thread, there are no installed fonts and no host transport, and SW Jackpot Over runs on a generated clip.',

  // The source is transparent where Backdrop is None: the backdrop shows it.
  showBackdrop: true,

  variants: {
    label: 'Plugin',
    default: initialVariant,
    options: [
      { id: 'source', name: 'SW Jackpot (source)', hint: 'JP01, FF_SOURCE: the casino on its own, or on nothing (Backdrop None).' },
      { id: 'over', name: 'SW Jackpot Over (effect)', hint: 'JP02, FF_EFFECT: the same over the clip, which can also be a reel symbol (Symbols: Clip).' },
    ],
  },

  // For SW Jackpot Over only.
  sources: ['scene', 'bars', 'grid', 'alpha', 'spot'],

  params: PARAMS,

  differences: [
    'This page runs the plugin rather than a port of it. jackpot-core.wasm is source/Jackpot.cpp — the plugin class: its constructor, clock, Play and Auto Play, the requests it makes, the wheel file, font resolution, the uploads and every uniform — and everything it calls: the five planners and their physics (Slots, Roulette, MoneyWheel, Craps on polyhedral’s dice engine, Lottery), the shower and its pile, WheelFace, FileBytes, SymbolArt, Typeface, Controls, Common, Shaders and Diag, with the FFGL SDK’s CFFGLPlugin, parameter bookkeeping, FFGLShader and FFGLScreenQuad, all compiled unmodified by emscripten (demo/tools/build-wasm.sh). Only the two registration files are left out; the page constructs the plugin itself, as the plugin’s harness does. The panel is read back from the plugin’s own declarations through the SDK’s host getters, and the numbers beside the sliders are Controls.cpp’s conversions, compiled in; their units and every tooltip are this page’s.',
    'The GLSL is the plugin’s: demo/shaders.js is spliced from source/Shaders.cpp, with the plugin’s own list of the eight programs it compiles (shaders::Programs()), and the repository’s verify script fails if a character differs. At start-up the page also requires each text the compiled plugin hands to glShaderSource to equal one of those programs, and refuses to compile anything else. The kit’s port() then changes the version line and adds the ES precision defaults. GL is emscripten’s WebGL2 library on this page’s context, not a GL 4.1 driver, and four entry points are the page’s (demo/wasm/gl_shim.cpp): glShaderSource, for that check and port, and glEnable, glDisable and glIsEnabled, because GL_PROGRAM_POINT_SIZE — state the plugin saves and restores each frame, as a plugin in Resolume must — does not exist in WebGL2. It reads as off here; the plugin draws no points.',
    'A play is planned on the page’s thread. The plugin plans roulette, the money wheel, craps and the lottery on a worker thread (std::async) and starts the play a frame or two after the press, back-dated to it; this WebAssembly has no threads, so the page switches on the plugin’s own synchronous planning (SetSynchronousForTest, the switch its harness uses), and the frame after a press waits for the planner. The line under the picture says how long that frame took. On the plugin’s Mac the planners take about 13 ms (roulette), 47 ms (the lottery, 49 balls) and 55 ms (the money wheel); the slot machine’s plan is a schedule and takes microseconds in both.',
    'No host transport. Nothing on this page calls SetBeatInfo, so Land On’s Beat and Bar fall on the plugin’s own bar clock at the 120 BPM it starts with (Transport.h), as they would in a host that never sends one. In Resolume they follow the composition’s tempo.',
    'No installed fonts. The plugin lists every font on the machine; a web page cannot read them, and the plugin’s own scan of the font folders finds none in the page’s in-memory file system, so Font offers Built-in alone. Font File takes a .ttf, .otf, .ttc or .otc you pick: it is written into that in-memory file system and loaded by the plugin’s own loader through stb_truetype. Font Name looks a family up in the same empty list, so any name falls back to the built-in face and is kept as typed.',
    'Wheel File is a .txt you pick, written into the page’s memory and read by the plugin’s own loader (FileBytes, WheelFace): the decoding, the markers, the emoji, the draw and the fallbacks are all the plugin’s. “Example” loads the repository’s tools/fixtures/showcase.txt, the README’s example, served byte for byte (verify.sh checks it) — and also sets Game to Money Wheel and Segments to 12, as the button says. In Resolume a file parameter is a path on the disk; here the path is in the page’s memory, so Copy link does not carry either file. Nothing you load leaves this page.',
    'The clock is the page’s: seconds from frame deltas, capped at a tenth of a second, handed to SetTime; the plugin’s own vote settles the unit (seconds) within four frames, as in Arena. Pause stops its clock and Step advances it 1/60 s. Restart is a backward jump, which the plugin takes as no time passing.',
    'The same Seed draws the same RESULTS here as in the plugin on a Mac or on Windows: they come from an integer generator or from Fixed Number. A spin’s path is not promised to be the same — WebAssembly has no fused multiply-add and its maths library is not Apple’s — exactly as the plugin’s AGENTS.md says of any two compilers. When this page was built it was compared with the plugin’s own harness, jptest (built by Apple clang on a Mac, with and without fused multiply-adds), frame for frame on jptest’s 60 fps clock through the same calls (demo/tools/crosscheck): eight plays — a slot jackpot, five reels at random, roulette on a fixed 17, the money wheel’s jackpot, a wheel file of twelve in order, craps on a fixed 11, the lottery, and a shower on its own — each showed the same result on both. The slot machine, roulette, the money wheel and craps also took the same paths: their frames matched jptest’s to within 2/255 on at least 99.6% of pixels, drawn here through Chrome on the Mac’s GPU and there through Apple’s OpenGL (99.5% through SwiftShader, Chrome’s software renderer); what differed was edges and filtering, not where anything was. The lottery’s 49 balls took another path through the drum within the first second — as two builds of the plugin itself do: jptest with and without fused multiply-adds disagree as well — and drew the same numbers; a few of the shower’s pieces bounced differently. That comparison does not run again.',
    'Fixed Number, Seed, Segments, Balls and Draw are FF_TYPE_INTEGER in the plugin and number fields here, clamped to the declared ranges. Play and Shower Now are FF_TYPE_EVENT; each button sends the press and the release, 1 then 0, as the harness does. The About block (a text and four buttons) is declared by the plugin and left off this panel.',
    'SW Jackpot Over’s clip is the kit’s generated one, premultiplied, or your own image or video; its whole texture is used (ClipScale 1), where a host may pad it. The clip shows with Backdrop None, or on the reels with Symbols Clip; at the constructor’s defaults (Backdrop Dark) it is covered, as it is in Resolume.',
    'Switching the Plugin dropdown is a new instance at that constructor’s defaults, as in Resolume; Symbols is shown as two rows of the same name because the effect’s list has one more element, Clip, and the kit’s dropdowns have a fixed list.',
    'The plugin has been played in Resolume Arena on a Mac (two plays) and checked in Arena on Windows (its README’s Status says exactly what was and was not). Its harness, jptest, renders the same classes headlessly and checks each game’s result against the one asked for, out of the pixels for the slots, roulette and the money wheel; that harness, not this page, is the reason to believe the results.',
  ],

  createRenderer: (gl) => {
    renderer = createRenderer(gl);
    return renderer;
  },
});

//===========================================================================
// The controls the kit has no type for, and the plugin's log.
//
// By inline style, not the `hidden` attribute: kit.css gives these elements a
// `display` of their own, which beats the attribute's user-agent rule.
//===========================================================================

if (mounted) {
  const params = mounted.params;
  const embed = query.has('embed') && query.get('embed') !== '0';
  let shownVariant = mounted.state.variant;

  // Copy link carries the plugin, and never a file path (it is in this page's memory).
  const toQuery = params.toQuery.bind(params);
  params.toQuery = () => {
    const q = toQuery();
    for (const p of PARAMS) if (p.ff.type === FF_TYPE_FILE) q.delete(p.id);
    if (mounted.state.variant === 'over') q.set('plugin', 'over');
    return q;
  };

  const show = (node, on) => { if (node) node.style.display = on ? '' : 'none'; };
  const rowOf = (p) => document.getElementById(`p-${p.id}`)?.closest('.prow')
    ?? [...document.querySelectorAll('.prow')].find((row) => row.querySelector('.prow__name')?.textContent === p.name);

  function showForVariant(which) {
    for (const p of PARAMS) if (p.only) show(rowOf(p), p.only === which);
    // A group with nothing of this plugin's in it (the source has no Over).
    for (const group of document.querySelectorAll('.pgroup')) {
      show(group, [...group.querySelectorAll('.prow')].some((row) => row.style.display !== 'none'));
    }
    const effect = which === 'over';
    for (const field of document.querySelectorAll('.transport__field')) {
      if (field.querySelector('.transport__label')?.textContent === 'Clip') show(field, effect);
    }
    show(document.querySelector('.transport__file'), effect);
  }

  document.addEventListener('demo:state', () => {
    const which = mounted.state.variant;
    if (which !== shownVariant) {
      shownVariant = which;
      // A new instance: that constructor's defaults, for Defaults too.
      params.defaults = { ...DEFAULTS[which] };
      params.reset();
    }
    if (!embed) showForVariant(which);
  });

  /** A file into the page's in-memory file system; its path, for the plugin. */
  function writeFile(name, bytes) {
    const dir = `/files/${Date.now().toString(36)}`;
    core.FS.mkdirTree(dir);
    const path = `${dir}/${name.replace(/[/\\]/g, '_')}`;
    core.FS.writeFile(path, bytes);
    return path;
  }

  if (!embed) {
    showForVariant(shownVariant);

    const button = (label, title) => {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = label;
      if (title) b.title = title;
      return b;
    };

    for (const p of PARAMS) {
      const row = rowOf(p);
      if (!row) continue;

      // FF_TYPE_EVENT: a press and a release.
      if (p.ff.type === FF_TYPE_EVENT) {
        const toggle = row.querySelector('.prow__toggle');
        const press = button(p.name, p.hint);
        press.className = 'prow__toggle';
        press.style.textAlign = 'center';
        press.addEventListener('click', () => {
          renderer?.press(p.ff.index);
          mounted.redraw();
        });
        toggle?.replaceWith(press);
        continue;
      }

      const field = row.querySelector('.prow__text');
      if (!field) continue;

      // FF_TYPE_INTEGER: a number field in the declared range.
      if (p.ff.type === FF_TYPE_INTEGER) {
        field.type = 'number';
        field.min = String(p.ff.min);
        field.max = String(p.ff.max);
        field.step = '1';
        field.inputMode = 'numeric';
        continue;
      }

      // FF_TYPE_TEXT: sent when committed, as a host sends text.
      if (p.ff.type === FF_TYPE_TEXT) {
        field.addEventListener('input', (event) => event.stopImmediatePropagation(), true);
        field.addEventListener('change', () => params.set(p.id, field.value));
        continue;
      }

      // FF_TYPE_FILE: the path, and a picker.
      if (p.ff.type === FF_TYPE_FILE) {
        field.readOnly = true;
        field.placeholder = 'no file';
        const cell = document.createElement('div');
        cell.style.display = 'flex';
        cell.style.gap = '6px';
        cell.style.minWidth = '0';
        field.replaceWith(cell);
        field.style.flex = '1 1 auto';
        field.style.minWidth = '0';
        const picker = document.createElement('input');
        picker.type = 'file';
        picker.accept = p.ff.extensions.map((e) => `.${e}`).join(',');
        picker.style.display = 'none';
        const choose = button('Choose…', `${p.ff.extensions.join(', ')} — read in this page, never uploaded`);
        const clear = button('×', 'No file');
        choose.addEventListener('click', () => picker.click());
        clear.addEventListener('click', () => params.set(p.id, ''));
        picker.addEventListener('change', async () => {
          const file = picker.files?.[0];
          picker.value = '';
          if (!file) return;
          params.set(p.id, writeFile(file.name, new Uint8Array(await file.arrayBuffer())));
        });
        cell.append(field, choose);
        if (p.name === 'Wheel File') {
          const example = button('Example', 'The README’s example wheel file (tools/fixtures/showcase.txt: twelve values, two jackpots, two jokers, four emoji). Also sets Game to Money Wheel and Segments to 12, the README’s picture.');
          example.addEventListener('click', async () => {
            const response = await fetch('./showcase.txt');
            const bytes = new Uint8Array(await response.arrayBuffer());
            const game = PARAMS.find((x) => x.name === 'Game');
            const segments = PARAMS.find((x) => x.name === 'Segments');
            if (game) params.set(game.id, game.elements.indexOf('Money Wheel'));
            if (segments) params.set(segments.id, '12');
            params.set(p.id, writeFile('showcase.txt', bytes));
          });
          cell.append(example);
        }
        cell.append(clear, picker);
      }
    }

    //-----------------------------------------------------------------------
    // What the plugin is playing, once it is at rest (never before: the plan
    // knows the result at the press), and the plugin's own log (Diag.cpp:
    // the play lines, fonts, wheel files). Both report; neither measures.
    //-----------------------------------------------------------------------
    const stage = document.querySelector('.stage');
    if (stage) {
      const status = document.createElement('p');
      status.className = 'stage__status';
      status.id = 'jackpot-play';
      status.setAttribute('aria-live', 'polite');
      const log = document.createElement('p');
      log.className = 'stage__status';
      log.id = 'jackpot-log';
      log.setAttribute('aria-live', 'off');
      log.style.fontFamily = 'ui-monospace, SFMono-Regular, Menlo, monospace';
      log.style.fontSize = '12px';
      log.style.whiteSpace = 'pre-wrap';
      stage.append(status, log);
      const KINDS = ['', ' — a win', ' — JACKPOT', ' — a near miss'];
      let lastLog = '';
      setInterval(() => {
        const play = renderer?.play();
        if (play) {
          const game = GAME_NAMES[play.game] ?? '';
          const planned = play.planFrameMs === null ? '' : ` The frame that planned it took ${play.planFrameMs.toFixed(0)} ms on this page’s thread.`;
          const paced = play.game === 0
            ? `the reels driven to their stops, at rest ${play.duration.toFixed(2)} s after Play`
            : `${play.natural.toFixed(2)} s of physics played at ${play.warp.toFixed(2)}× to come to rest ${play.duration.toFixed(2)} s after Play`;
          const chosen = PARAMS.find((x) => x.name === 'Game');
          const showerOnly = chosen && chosen.elements[Math.round(params.get(chosen.id))] === 'Shower Only';
          status.textContent = showerOnly
            ? `Shower Only: Play or Shower Now throws the chips and coins. ${play.pieces} in the air or in the pile.`
            : play.game < 0
            ? 'At rest before a play. Press Play.'
            : !play.finished
              ? `Play ${play.plays} (${game}): playing…`
              : `Play ${play.plays} (${game}): ${play.text || '—'}${KINDS[play.kind] ?? ''}. ${paced[0].toUpperCase()}${paced.slice(1)}.${planned}`;
        }
        const logText = renderer?.log() ?? '';
        if (logText === lastLog) return;
        lastLog = logText;
        const lines = logText.trim().split('\n').filter(Boolean).map((l) => l.replace(/^\S+ (INFO |WARN |ERROR) jackpot: /, '$1 '));
        log.textContent = lines.length
          ? `The plugin’s own log (Diag.cpp, in this page’s memory):\n${lines.slice(-2).join('\n')}`
          : '';
      }, 250);
    }
  }
}
