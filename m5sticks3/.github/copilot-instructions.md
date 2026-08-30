# Code Naming Conventions

Always adhere strictly to the following naming conventions when writing or refactoring code:

- **Global Variables**: `lowerCamelCase` (e.g., `globalApiUrl`)
- **Class-Level Variables / Properties**: Single underscore prefix followed by lowerCamelCase `_lowerCamelCase` (e.g., `_instanceCount`, `_userName`)
- **Local Variables (inside functions/methods/procedures)**: Double underscore prefix followed by lowerCamelCase `__lowerCamelCase` (e.g., `__tempValue`, `__itemIndex`)
- **Constants & Defines**: `ALL_UPPERCASE` with underscores separating words (e.g., `MAX_RETRY_LIMIT`, `API_KEY`)