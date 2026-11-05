// Run in a real browser using agent-browser eval --stdin (see site/README.md).
(async () => {
  const { LESSONS, PRESETS } = await import(new URL('./assets/content.js', location.href));
  const results = [];
  const $ = (s) => document.querySelector(s);
  const assert = (ok, name) => { if (!ok) throw new Error(name); results.push(name); };
  const paint = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
  const waitFor = async (predicate) => {
    const deadline = performance.now() + 20000;
    while (!predicate()) {
      if (performance.now() > deadline) throw new Error('Timed out waiting for UI');
      await new Promise((r) => setTimeout(r, 30));
    }
  };
  const go = async (id) => { location.hash = id; await paint(); };
  for (const language of ['zh', 'en']) {
    if (document.documentElement.lang.startsWith(language) === false) { $('#language').click(); await paint(); }
    for (const lesson of LESSONS) {
      await go(lesson.id);
      assert($('#lesson-title').textContent === lesson[language].title, `${language}/${lesson.id}: title`);
      assert(document.querySelectorAll('#lessons [aria-current=step]').length === 1, `${language}/${lesson.id}: current lesson`);
      assert(document.documentElement.scrollWidth <= innerWidth + 1, `${language}/${lesson.id}: no page overflow`);
      assert(Math.max($('.stage').getBoundingClientRect().bottom, $('#lesson-panel').getBoundingClientRect().bottom) <= $('.curriculum').getBoundingClientRect().top + 1, `${language}/${lesson.id}: curriculum does not overlap workspace`);
      $('#tab-code').click();
      assert(!$('#pane-code').hidden && $('#pane-learn').hidden, `${language}/${lesson.id}: code tab`);
      assert($('#code').textContent === (lesson[language].code?.text || (language === 'zh' ? '本课没有代码。' : 'This lesson has no code.')), `${language}/${lesson.id}: code content`);
      $('#tab-pins').click();
      assert(!$('#pane-pins').hidden, `${language}/${lesson.id}: pins tab`);
      $('#tab-learn').click();
      if (lesson.replay) {
        $('#play').click();
        await waitFor(() => $('#play').textContent.includes(language === 'zh' ? '停止' : 'Stop'));
        $('#play').click();
        assert($('#play').textContent.includes(language === 'zh' ? '播放' : 'Play'), `${language}/${lesson.id}: stop replay`);
      }
    }
  }
  await go('matrix');
  const cells = [...document.querySelectorAll('[data-pixel]')];
  assert(cells.length === 96, 'matrix: 96 editable pixels');
  for (const preset of ['00', '01', '02', '03']) {
    $(`[data-preset="${preset}"]`).click();
    assert(cells.every((b, i) => $('#matrix-grid').children[i].getAttribute('aria-pressed') === String(!!PRESETS[preset][i])), `matrix: preset ${preset}`);
  }
  $('[data-preset="00"]').click();
  $('[data-pixel="0"]').click();
  $('[data-pixel="95"]').click();
  assert($('#matrix-payload').textContent === '10 80 00 00 00 00 00 00 00 00 00 00 01', 'matrix: corner frame payload');
  $('#matrix-send').click();
  await waitFor(() => !$('#matrix-send').disabled);
  assert($('#terminal').textContent.includes('OK:RAW'), 'matrix: complete simulated GATT round trip');
  await go('at');
  $('#play').click();
  await waitFor(() => $('#play').textContent.includes('Play') || $('#play').textContent.includes('播放'));
  assert($('#terminal').textContent.includes('+NOSUCH:ERROR'), 'AT: complete success and error replay');
  await go('board');
  const labelButton = $('[data-board="labels"]');
  labelButton.click();
  assert(document.querySelectorAll('.hotspot').length === 0, 'board: labels off');
  labelButton.click();
  assert(document.querySelectorAll('.hotspot').length === 6, 'board: labels on');
  document.querySelector('.hotspot[data-id="bl"]').dispatchEvent(new MouseEvent('click', { bubbles: true }));
  assert(!$('#part-popover').hidden && $('#part-text').textContent.includes('BL616CL'), 'board: component details');
  $('#close-part').click();
  assert($('#part-popover').hidden, 'board: close details');
  const before = document.querySelector('.scene').innerHTML;
  $('[data-board="right"]').click();
  await new Promise((r) => setTimeout(r, 450));
  assert(document.querySelector('.scene').innerHTML !== before, 'board: rotation changes geometry');
  $('[data-board="reset"]').click();
  await paint();
  assert($('#board-svg text.board-logo').getAttribute('font-size') === '2.6', 'board: silkscreen keeps millimetre-scale type');
  const font = parseFloat(getComputedStyle($('#board-svg text.board-logo')).fontSize);
  assert(font < 4, 'board: no HTML brand-style leakage');
  return { viewport: [innerWidth, innerHeight], passed: results.length, results };
})()
