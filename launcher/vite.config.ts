import { defineConfig } from "vite";

// Tauri expects a fixed port in development and a plain static build in dist/.
export default defineConfig({
  clearScreen: false,
  server: { port: 1420, strictPort: true },
  build: { target: "es2021", outDir: "dist", emptyOutDir: true },
});
