function themeSwitcher() {
  return {
    isDark: false,

    init() {
      const saved = localStorage.getItem('theme') || 'light';
      this.isDark = saved === 'dark';
      this._apply(saved);
    },

    toggleTheme() {
      this.isDark = !this.isDark;
      const theme = this.isDark ? 'dark' : 'light';
      localStorage.setItem('theme', theme);
      this._applyWithAnimation(theme);
    },

    _apply(theme) {
      document.documentElement.setAttribute('data-bs-theme', theme);
    },

    _applyWithAnimation(theme) {
      const overlay = document.getElementById('theme-waterdrop-overlay');
      if (!overlay) { this._apply(theme); return; }
      overlay.classList.add('active');
      overlay.style.clipPath = 'circle(150% at 50% 50%)';
      setTimeout(() => {
        this._apply(theme);
        overlay.style.clipPath = 'circle(0% at 50% 50%)';
        setTimeout(() => overlay.classList.remove('active'), 700);
      }, 50);
    }
  };
}
