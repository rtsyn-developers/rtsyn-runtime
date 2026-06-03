# RTSyn Runtime

RTSyn Runtime is the C runtime layer for executing RTSyn nodes and their connections.

## Public API

The public API is declared in:

- `include/rtsyn/runtime.h`: runtime creation, destruction, cycle execution, node management, and connection management.

## Basic Usage

```c
#include "include/rtsyn/runtime.h"

int main(void) {
  rtsyn_runtime_t *runtime = rtsyn_runtime_create();

  if (runtime == NULL) {
    return 1;
  }

  rtsyn_runtime_destroy(runtime);
  return 0;
}
```

## Generating Documentation

Run:

```sh
xmake doxygen
```

Then open `build/html/index.html`.
