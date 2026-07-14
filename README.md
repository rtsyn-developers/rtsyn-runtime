# RTSyn Runtime

## Usage

### Update

Make sure you have last version of the dependencies:

```bash
xmake repo update-repo
xmake require --upgrade
```

For development you may need to run:

```bash
xmake require --upgrade -fy <dependency_name>
```

### Compiling

For compiling:

```bash
xmake
```

### Tests

For running test:

```bash
xmake test
```

For enabling valgrind, before running tests:

```bash
xmake f --valgrind=y
```

For disabling valgrind, just replace `y` for `n`.

### Local development

If you want to test your changes locally from different parts of RTSyn, export the path where you have all the repos:

```bash
export RTSYN_WORKSPACE=<PATH>
```

> [!WARNING]
> This expects you also the `rtsyn-xmake-repo`.

### Cleaning

To remove all generated build artifacts:

```bash
xmake clean --all
```

To also reset cached configuration and tool detection:

```bash
xmake f -c
```
