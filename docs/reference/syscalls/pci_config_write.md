# zx_pci_config_write

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

TODO(fxbug.dev/32938)

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_pci_config_write(zx_handle_t handle,
                                uint16_t offset,
                                size_t width,
                                uint32_t val);
```

## DESCRIPTION

TODO(fxbug.dev/32938)

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_PCI_DEVICE** and have **ZX_RIGHT_READ** and have **ZX_RIGHT_WRITE**.

## RETURN VALUE

TODO(fxbug.dev/32938)

## ERRORS

TODO(fxbug.dev/32938)

## SEE ALSO


TODO(fxbug.dev/32938)
