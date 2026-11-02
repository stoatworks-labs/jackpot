// The browser half of demo/tools/crosscheck/crosscheck.py: the page's own
// jackpot-core.wasm driven exactly as tools/jptest's Rig drives the plugin
// for `--film`, so the two can be compared frame for frame.
//
// Evaluated by crosscheck.py in a page of the demo's origin (it imports the
// served module, never a copy). Not served to visitors: demo/.assetsignore
// keeps tools/ off the site.
//
// What is the same as jptest, deliberately:
//   - SetSynchronousForTest(true) (glue.cpp always sets it) and
//     SetClockScaleForTest(1.0): host seconds are frame / 60, from frame 0;
//   - Play pressed (1 then 0) before frame 0 is rendered, as `--play 0`;
//   - before every frame: the host framebuffer bound, the viewport, a clear
//     to transparent black, then ProcessOpenGL;
//   - the parameters set by name, options by element name, through the
//     plugin's own setters, as jptest's applySetting does.
// What is not: jptest renders into an RGBA32F texture and rounds each
// channel with lround( clamp( v ) * 255 ); here the WebGL2 drawing buffer is
// RGBA8, so the GPU rounds. Equal floats can therefore differ by one level.
(() => {
  const FF_TYPE_OPTION = 11;
  const FF_TYPE_FILE = 14;
  const FF_TYPE_TEXT = 100;

  async function run(scenario) {
    const [{ default: createJackpot }, S, { port }] = await Promise.all([
      import('/jackpot-core.js'), import('/shaders.js'), import('/vendor/gl.js'),
    ]);
    // The page's own check: only the plugin's programs, as shaders.js has them.
    const programs = S.PROGRAMS.map((p) => ({
      vertex: p.vertex.map((x) => S[x]).join(''),
      fragment: p.fragment.map((x) => S[x]).join(''),
    }));
    let refused = 0;
    const core = await createJackpot({
      jackpotShaderSource: (stage, text) => {
        if (!programs.some((p) => p[stage] === text)) { refused += 1; return ''; }
        return port(text, { stage });
      },
      print: () => {},
    });
    const { width, height } = scenario;
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const gl = canvas.getContext('webgl2', {
      alpha: true, antialias: false, depth: false, stencil: false, premultipliedAlpha: true, preserveDrawingBuffer: true,
    });
    const handle = core.GL.registerContext(gl, { majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: false });
    core.GL.makeContextCurrent(handle);
    core._jp_prepare_log();
    const instance = core._jp_new(scenario.effect ? 1 : 0);
    gl.viewport(0, 0, width, height);
    if (!core._jp_init_gl(instance, width, height)) throw new Error(`InitGL failed (${refused} shaders refused)`);
    core._jp_set_clock_scale_for_test(instance, 1.0);

    const text = (p) => core.UTF8ToString(p);
    const names = {};
    for (let i = 0; i < core._jp_param_count(instance); i += 1) names[text(core._jp_param_name(instance, i))] = i;
    for (const [name, value] of scenario.settings) {
      const index = names[name];
      if (index === undefined) throw new Error(`no parameter called '${name}'`);
      const type = core._jp_param_type(instance, index);
      let v = value;
      if (name === 'Wheel File' && value) {
        const bytes = new Uint8Array(await (await fetch(`/${value}`)).arrayBuffer());
        core.FS.mkdirTree('/files/crosscheck');
        v = `/files/crosscheck/${value}`;
        core.FS.writeFile(v, bytes);
      }
      if (type === FF_TYPE_TEXT || type === FF_TYPE_FILE) {
        const pointer = core.stringToNewUTF8(String(v));
        core._jp_set_text(instance, index, pointer);
        core._free(pointer);
        continue;
      }
      let number = Number(v);
      if (!Number.isFinite(number) && type === FF_TYPE_OPTION) {
        const count = core._jp_param_element_count(instance, index);
        const e = [...Array(count).keys()].find((k) => text(core._jp_param_element_name(instance, index, k)) === v);
        if (e === undefined) throw new Error(`'${v}' is not one of ${name}'s options`);
        number = core._jp_param_element_value(instance, index, e);
      }
      core._jp_set_float(instance, index, Math.fround(number));
    }

    const play = names.Play;
    const frames = [];
    const pixels = new Uint8Array(width * height * 4);
    const flipped = new Uint8Array(width * height * 4);
    for (let f = 0; f < scenario.frames; f += 1) {
      if (scenario.plays.includes(f)) {
        core._jp_set_float(instance, play, 1);
        core._jp_set_float(instance, play, 0);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      if (!core._jp_process(instance, f / 60, 0, 0, 0)) throw new Error(`ProcessOpenGL failed at frame ${f}`);
      if (scenario.sample.includes(f)) {
        gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
        const row = width * 4;
        for (let y = 0; y < height; y += 1) flipped.set(pixels.subarray((height - 1 - y) * row, (height - y) * row), y * row);
        let binary = '';
        for (let i = 0; i < flipped.length; i += 0x8000) binary += String.fromCharCode(...flipped.subarray(i, i + 0x8000));
        frames.push({ frame: f, rgba: btoa(binary) });
      }
    }
    const result = {
      text: text(core._jp_play_text(instance)),
      warp: core._jp_play_warp(instance),
      natural: core._jp_play_natural(instance),
      plays: core._jp_plays_started(instance),
      refused,
      renderer: (() => {
        const info = gl.getExtension('WEBGL_debug_renderer_info');
        return gl.getParameter(info ? info.UNMASKED_RENDERER_WEBGL : gl.RENDERER);
      })(),
      frames,
    };
    core._jp_delete(instance);
    return result;
  }

  window.jackpotCrosscheck = run;
})();
