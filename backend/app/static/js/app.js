(function () {
  const button = document.querySelector('[data-menu-toggle]');
  const menu = document.querySelector('[data-menu]');
  if (button && menu) {
    button.addEventListener('click', () => menu.classList.toggle('open'));
  }

  async function refreshRecent() {
    const target = document.querySelector('[data-live-recent]');
    if (!target) return;
    try {
      const response = await fetch('/api/transactions/recent');
      if (!response.ok) return;
      const rows = await response.json();
      if (!rows.length) return;
      target.innerHTML = rows.map(row => `
        <div class="list-card">
          <div><strong>${escapeHtml(row.student)}</strong><small>${escapeHtml(row.classroom || 'Sem turma')} · ${escapeHtml(row.kind)}</small></div>
          <div class="score ${row.credits >= 0 ? 'positive' : 'negative'}">${row.credits >= 0 ? '+' : ''}${row.credits}</div>
        </div>
      `).join('');
    } catch (error) {
      // Silent failure: the dashboard must not annoy teachers during unstable connections.
    }
  }

  function escapeHtml(value) {
    return String(value || '').replace(/[&<>"]/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[char]));
  }

  if (document.querySelector('[data-live-recent]')) {
    setInterval(refreshRecent, 5000);
  }
})();
