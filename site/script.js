/* Glustick Link — Site JS */

// ── Progressive enhancement — opt into fade animations ──────────────────────
document.body.classList.add('js-fade-ready');

// ── Sticky nav ──────────────────────────────────────────────────────────────
const nav = document.getElementById('nav');
window.addEventListener('scroll', () => {
  nav.classList.toggle('scrolled', window.scrollY > 20);
}, { passive: true });

// ── Fade-in on scroll ───────────────────────────────────────────────────────
const fadeEls = document.querySelectorAll('[data-fade]');

if ('IntersectionObserver' in window) {
  const io = new IntersectionObserver((entries) => {
    entries.forEach(e => {
      if (e.isIntersecting) {
        e.target.classList.add('visible');
        io.unobserve(e.target);
      }
    });
  }, { threshold: 0.12, rootMargin: '0px 0px -40px 0px' });

  fadeEls.forEach(el => io.observe(el));
} else {
  fadeEls.forEach(el => el.classList.add('visible'));
}

// ── Tab switcher ────────────────────────────────────────────────────────────
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    const target = btn.dataset.tab;
    const parent = btn.closest('div');

    parent.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');

    const section = btn.closest('.section, main');
    section.querySelectorAll('.tab-panel').forEach(p => p.classList.remove('active'));
    const panel = document.getElementById('tab-' + target);
    if (panel) panel.classList.add('active');
  });
});

// ── Copy to clipboard ───────────────────────────────────────────────────────
document.querySelectorAll('.copy-btn').forEach(btn => {
  btn.addEventListener('click', async () => {
    const targetId = btn.dataset.code;
    const pre = document.getElementById(targetId);
    if (!pre) return;

    const text = pre.textContent;
    try {
      await navigator.clipboard.writeText(text);
      btn.textContent = 'Copied!';
      btn.classList.add('copied');
      setTimeout(() => {
        btn.textContent = 'Copy';
        btn.classList.remove('copied');
      }, 2000);
    } catch {
      btn.textContent = 'Failed';
      setTimeout(() => { btn.textContent = 'Copy'; }, 2000);
    }
  });
});

// ── OLED state animation ────────────────────────────────────────────────────
// Cycles: boot (1.5s) → provisioning (3s) → joining (2s) → idle (3s) → loop
const oledStates = [
  { id: 'oled-boot',  duration: 1800 },
  { id: 'oled-prov',  duration: 3200 },
  { id: 'oled-join',  duration: 2200 },
  { id: 'oled-idle',  duration: 3000 },
];

let currentState = 0;

function showOledState(idx) {
  const all = document.querySelectorAll('.oled-state');
  all.forEach(el => el.classList.add('oled-hidden'));

  const target = document.getElementById(oledStates[idx].id);
  if (target) target.classList.remove('oled-hidden');
}

function cycleOled() {
  showOledState(currentState);
  const duration = oledStates[currentState].duration;
  currentState = (currentState + 1) % oledStates.length;
  setTimeout(cycleOled, duration);
}

// Start cycling after a short delay so first paint is settled
setTimeout(cycleOled, 600);

// ── Mobile nav ──────────────────────────────────────────────────────────────
const menuBtn = document.getElementById('menuBtn');
const navLinks = document.querySelector('.nav-links');

if (menuBtn && navLinks) {
  menuBtn.addEventListener('click', () => {
    const open = navLinks.style.display === 'flex';
    navLinks.style.display = open ? '' : 'flex';
    navLinks.style.flexDirection = 'column';
    navLinks.style.position = 'absolute';
    navLinks.style.top = '64px';
    navLinks.style.left = '0';
    navLinks.style.right = '0';
    navLinks.style.background = 'rgba(8, 13, 24, 0.98)';
    navLinks.style.padding = '16px 24px 24px';
    navLinks.style.borderBottom = '1px solid var(--border)';
    navLinks.style.gap = '16px';
    if (open) { navLinks.style.display = 'none'; }
  });

  navLinks.querySelectorAll('a').forEach(link => {
    link.addEventListener('click', () => { navLinks.style.display = 'none'; });
  });
}
