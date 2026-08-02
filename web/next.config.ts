import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // The font route reads these TTFs from disk at runtime; without an explicit
  // include the serverless bundle on Vercel omits them and subsetting 500s.
  outputFileTracingIncludes: {
    "/api/font": ["./assets/fonts/**/*"],
  },
};

export default nextConfig;
