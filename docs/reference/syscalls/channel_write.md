# zx_channel_write

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

Write a message to a channel.

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```c
#include <zircon/syscalls.h>

zx_status_t zx_channel_write(zx_handle_t handle,
                             uint32_t options,
                             const void* bytes,
                             uint32_t num_bytes,
                             const zx_handle_t* handles,
                             uint32_t num_handles);
```

## DESCRIPTION

`zx_channel_write()` attempts to write a message of *num_bytes*
bytes and *num_handles* handles to the channel specified by
*handle*.  The pointers *handles* and *bytes* may be NULL if their
respective sizes are zero.

On success, all *num_handles* of the handles in the *handles* array
are attached to the message and will become available to the reader
of that message from the opposite end of the channel.

All handles are discarded and no longer available to the caller, on
success or failure. Use [`zx_channel_write_etc()`] if handles need
to be preserved by the sender.

It is invalid to include *handle* (the handle of the channel being written
to) in the *handles* array (the handles being sent in the message).

The maximum number of handles which may be sent in a message is
**ZX_CHANNEL_MAX_MSG_HANDLES**, which is 64.

The maximum number of bytes which may be sent in a message is
**ZX_CHANNEL_MAX_MSG_BYTES**, which is 65536.


## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_CHANNEL** and have **ZX_RIGHT_WRITE**.

Every entry of *handles* must have **ZX_RIGHT_TRANSFER**.

## RETURN VALUE

`zx_channel_write()` returns **ZX_OK** on success.

## ERRORS

**ZX_ERR_BAD_HANDLE**  *handle* is not a valid handle, any element in
*handles* is not a valid handle, or there are repeated handles among the
handles in the *handles* array.

**ZX_ERR_WRONG_TYPE**  *handle* is not a channel handle.

**ZX_ERR_INVALID_ARGS**  *bytes* is an invalid pointer, *handles*
is an invalid pointer, or *options* is nonzero.

**ZX_ERR_NOT_SUPPORTED**  *handle* was found in the *handles* array, or
one of the handles in *handles* was *handle* (the handle to the
channel being written to).

**ZX_ERR_ACCESS_DENIED**  *handle* does not have **ZX_RIGHT_WRITE** or
any element in *handles* does not have **ZX_RIGHT_TRANSFER**.

**ZX_ERR_PEER_CLOSED**  The other side of the channel is closed.

**ZX_ERR_NO_MEMORY**  Failure due to lack of memory.
There is no good way for userspace to handle this (unlikely) error.
In a future build this error will no longer occur.

**ZX_ERR_OUT_OF_RANGE**  *num_bytes* or *num_handles* are larger than the
largest allowable size for channel messages.

## NOTES

*num_handles* is a count of the number of elements in the *handles*
array, not its size in bytes.

The byte size limitation on messages is not yet finalized.

## SEE ALSO

 - [`zx_channel_call()`]
 - [`zx_channel_create()`]
 - [`zx_channel_read()`]
 - [`zx_channel_read_etc()`]
 - [`zx_channel_write_etc()`]
 - [`zx_handle_close()`]
 - [`zx_handle_duplicate()`]
 - [`zx_handle_replace()`]
 - [`zx_object_wait_async()`]
 - [`zx_object_wait_many()`]
 - [`zx_object_wait_one()`]

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_channel_call()`]: channel_call.md
[`zx_channel_create()`]: channel_create.md
[`zx_channel_read()`]: channel_read.md
[`zx_channel_read_etc()`]: channel_read_etc.md
[`zx_channel_write_etc()`]: channel_write_etc.md
[`zx_handle_close()`]: handle_close.md
[`zx_handle_duplicate()`]: handle_duplicate.md
[`zx_handle_replace()`]: handle_replace.md
[`zx_object_wait_async()`]: object_wait_async.md
[`zx_object_wait_many()`]: object_wait_many.md
[`zx_object_wait_one()`]: object_wait_one.md
