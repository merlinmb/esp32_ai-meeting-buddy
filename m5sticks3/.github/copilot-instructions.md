# Code Naming Conventions

Always adhere strictly to the following naming conventions when writing or refactoring code:

- **Global Variables**: `lowerCamelCase` (e.g., `globalApiUrl`)
- **Class-Level Variables / Properties**: Single underscore prefix followed by lowerCamelCase `_lowerCamelCase` (e.g., `_instanceCount`, `_userName`)
- **Local Variables (inside functions/methods/procedures)**: Double underscore prefix followed by lowerCamelCase `__lowerCamelCase` (e.g., `__tempValue`, `__itemIndex`)
- **Constants & Defines**: `ALL_UPPERCASE` with underscores separating words (e.g., `MAX_RETRY_LIMIT`, `API_KEY`)

# Embedded C/C++ Code Conventions

When writing or refactoring C/C++ code (including Arduino/PlatformIO):

- **Global Variables**: `lowerCamelCase` (e.g., `wifiStatus`, `sensorBuffer`)
- **Class Member Variables / Struct Fields**: Single underscore prefix followed by lowerCamelCase `_lowerCamelCase` (e.g., `_baudRate`, `_isInitialized`)
- **Local Variables (inside functions/ISR/procedures)**: Double underscore prefix followed by lowerCamelCase `__lowerCamelCase` (e.g., `__currentTick`, `__tempData`)
- **Macros / Defines / Constants**: `ALL_UPPERCASE` with underscores (e.g., `LED_PIN`, `MAX_BUFFER_SIZE`)