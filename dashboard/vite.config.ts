import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// base './' so the static build works from any path (GitHub Pages, file server).
export default defineConfig({
  plugins: [react()],
  base: './',
  server: {
    proxy: { '/api': 'http://localhost:8787' },
  },
  build: { chunkSizeWarningLimit: 1200 },
});
