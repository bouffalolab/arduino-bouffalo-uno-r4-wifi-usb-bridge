export function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

export function visibleBytes(text) {
  return text.replace(/\r/g, '\\r').replace(/\n/g, '\\n');
}

// PhoneMatrix: opcode 0x10, then 96 row-major bits, most significant bit first.
export function packFrame(pixels) {
  const bytes = [0x10];
  for (let i = 0; i < 96; i += 8) {
    let byte = 0;
    for (let bit = 0; bit < 8; bit++) byte |= pixels[i + bit] << (7 - bit);
    bytes.push(byte);
  }
  return bytes.map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
}
