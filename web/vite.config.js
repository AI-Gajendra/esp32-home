import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  root: 'client',
  plugins: [react()],
  build: {
    outDir: '../dist',
    emptyOutDir: true,
  },
  server: {
    // `npm run dev` proxies the API to the running server so the dashboard
    // can be developed against real data.
    proxy: {
      '/api': 'http://127.0.0.1:3123',
    },
  },
});
