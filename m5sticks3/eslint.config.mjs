import typescriptEslint from "@typescript-eslint/eslint-plugin";
import tsParser from "@typescript-eslint/parser";

export default [
  {
    files: ["**/*.ts", "**/*.tsx", "**/*.js", "**/*.jsx"],
    languageOptions: {
      parser: tsParser,
    },
    plugins: {
      "@typescript-eslint": typescriptEslint,
    },
    rules: {
      "@typescript-eslint/naming-convention": [
        "error",
        {
          "selector": "variable",
          "modifiers": ["global"],
          "format": ["camelCase"]
        },
        {
          "selector": "classProperty",
          "format": ["camelCase"],
          "leadingUnderscore": "require"
        },
        {
          "selector": "variable",
          "modifiers": ["local"],
          "format": null,
          "custom": {
            "regex": "^__[a-z][a-zA-Z0-9]*$",
            "match": true
          }
        },
        {
          "selector": "variable",
          "modifiers": ["const"],
          "format": ["UPPER_CASE"]
        }
      ]
    }
  }
];