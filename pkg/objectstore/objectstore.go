package objectstore

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"
)

// FileMeta describes a single logical file in the remote store.
type FileMeta struct {
	Path         string
	Size         int64
	ETag         string
	LastModified time.Time
	IsDir        bool
}

var ErrNotFound = errors.New("object not found")

// NotFoundError conveys that a specific object key was not found in the store.
type NotFoundError struct {
	Key string
}

func (e NotFoundError) Error() string {
	if e.Key == "" {
		return "object not found"
	}
	return fmt.Sprintf("%s: not found", e.Key)
}

func (e NotFoundError) Unwrap() error {
	return ErrNotFound
}

// IsNotFound reports whether err represents a missing remote object.
func IsNotFound(err error) bool {
	return errors.Is(err, ErrNotFound)
}

// ObjectStore abstracts the object storage provider used by RemoteFS.
type ObjectStore interface {
	// Head returns metadata for a single object. The caller is expected to pass
	// normalized, slash-separated paths relative to the configured root.
	Head(ctx context.Context, key string) (FileMeta, error)
	// List returns metadata for all objects that are direct children of the
	// provided key. The key may be "", representing the virtual root.
	List(ctx context.Context, key string) ([]FileMeta, error)
	// Download streams the content of a single object into dst. Implementations
	// must return io.EOF once the content is drained.
	Download(ctx context.Context, key string, dst io.WriterAt) error
}
