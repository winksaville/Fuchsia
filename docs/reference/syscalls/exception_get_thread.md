# zx_exception_get_thread

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Create a handle for the exception's thread.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_exception_get_thread(zx_handle_t handle, zx_handle_t* out);
```

## DESCRIPTION

*handle* is the exception handle.

*out* will be filled with a new handle to the exception thread. This handle
will have the same rights as the task given to
[`zx_task_create_exception_channel()`].

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_EXCEPTION**.

## RETURN VALUE

`zx_exception_get_thread()` returns **ZX_OK** on success.
In the event of failure, a negative error value is returned.

## ERRORS

**ZX_ERR_BAD_HANDLE** *handle* is not a valid handle.

**ZX_ERR_NO_MEMORY**  Failed to allocate memory for a new handle.

**ZX_ERR_WRONG_TYPE**  *handle* is not an exception.

## SEE ALSO

 - [exceptions](/docs/concepts/kernel/exceptions.md)
 - [`zx_exception_get_process()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_exception_get_process()`]: exception_get_process.md
[`zx_task_create_exception_channel()`]: task_create_exception_channel.md
