package remotefs

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path"
	"path/filepath"
	"strings"
	"sync"

	"example.com/s3rofs/pkg/cache"
	"example.com/s3rofs/pkg/objectstore"
)

// Config describes the virtual filesystem behaviour.
type Config struct {
	LocalRoot string
	CacheDir  string
	CacheSize int64
}

// FileSystem translates local style paths into remote object storage calls.
type FileSystem struct {
	store     objectstore.ObjectStore
	cfg       Config
	cache     *cache.Cache
	localRoot string

	metaMu sync.RWMutex
	meta   map[string]objectstore.FileMeta
}

// NotFoundError is returned when the requested local path does not exist in the
// remote backing store.
type NotFoundError struct {
	Path string
}

func (e NotFoundError) Error() string {
	if e.Path == "" {
		return "No such file or directory"
	}
	return fmt.Sprintf("%s: No such file or directory", e.Path)
}

// IsNotFound reports whether err is a NotFoundError.
func IsNotFound(err error) bool {
	var target NotFoundError
	return errors.As(err, &target)
}

// New constructs a RemoteFS facade backed by the provided store and runtime
// configuration. It also ensures the cache directory and local root are
// normalized so later path checks remain cheap.
func New(store objectstore.ObjectStore, cfg Config) (*FileSystem, error) {
	cacheDir := cfg.CacheDir
	if cacheDir == "" {
		cacheDir = filepath.Join(os.TempDir(), "remotefs-cache")
	}
	cfg.CacheDir = cacheDir
	c, err := cache.New(cacheDir, cfg.CacheSize)
	if err != nil {
		return nil, err
	}
	root := strings.TrimSpace(cfg.LocalRoot)
	if root != "" {
		root = filepath.Clean(root)
		if root == "." {
			root = ""
		}
	}
	cfg.LocalRoot = root
	fs := &FileSystem{
		store: store,
		cfg:   cfg,
		cache: c,
	}
	fs.localRoot = root
	return fs, nil
}

// LocalRoot returns the canonical local root configured for the filesystem.
func (fs *FileSystem) LocalRoot() string {
	if fs.localRoot == "" {
		return string(os.PathSeparator)
	}
	return fs.localRoot
}

// joinLocal stitches the sanitized relative path back together with the
// configured local root so errors can surface the path the user expects.
func (fs *FileSystem) joinLocal(rel string) string {
	if fs.localRoot == "" {
		if rel == "" {
			return string(os.PathSeparator)
		}
		return filepath.Join(string(os.PathSeparator), filepath.FromSlash(rel))
	}
	if rel == "" {
		return fs.localRoot
	}
	return filepath.Join(fs.localRoot, filepath.FromSlash(rel))
}

// sanitize normalizes and ensures the path stays under the configured root.
func (fs *FileSystem) sanitize(local string) (string, error) {
	local = strings.TrimSpace(local)
	if local == "" {
		return "", fmt.Errorf("empty path")
	}
	target := filepath.Clean(local)
	if fs.localRoot != "" {
		root := fs.localRoot
		if target != root {
			prefix := root + string(os.PathSeparator)
			if !strings.HasPrefix(target, prefix) {
				return "", fmt.Errorf("path %s outside of %s", target, root)
			}
			target = strings.TrimPrefix(target, prefix)
		} else {
			target = ""
		}
	} else {
		// The virtual filesystem expects slash separated paths irrespective
		// of the OS, so strip any leading separators and normalize the rest.
		target = strings.TrimLeft(target, string(os.PathSeparator))
	}
	rel := path.Clean(filepath.ToSlash(target))
	rel = strings.TrimPrefix(rel, "/")
	if rel == "." {
		rel = ""
	}
	return rel, nil
}

// Stat returns file metadata matching os.Stat semantics.
func (fs *FileSystem) Stat(ctx context.Context, local string) (objectstore.FileMeta, error) {
	rel, err := fs.sanitize(local)
	if err != nil {
		return objectstore.FileMeta{}, err
	}
	if rel == "" {
		return objectstore.FileMeta{Path: "", IsDir: true}, nil
	}
	absPath := fs.joinLocal(rel)
	if meta, ok := fs.cachedMeta(rel); ok {
		return meta, nil
	}
	meta, err := fs.store.Head(ctx, rel)
	if err == nil {
		return meta, nil
	}
	if !objectstore.IsNotFound(err) {
		return objectstore.FileMeta{}, err
	}
	entries, listErr := fs.store.List(ctx, rel)
	if listErr == nil && len(entries) > 0 {
		return objectstore.FileMeta{
			Path:  rel,
			IsDir: true,
		}, nil
	}
	if listErr != nil && !objectstore.IsNotFound(listErr) {
		return objectstore.FileMeta{}, listErr
	}
	return objectstore.FileMeta{}, NotFoundError{Path: absPath}
}

// ReadDir fetches directory contents.
func (fs *FileSystem) ReadDir(ctx context.Context, local string) ([]objectstore.FileMeta, error) {
	rel, err := fs.sanitize(local)
	if err != nil {
		return nil, err
	}
	items, listErr := fs.store.List(ctx, rel)
	if listErr != nil {
		if objectstore.IsNotFound(listErr) || rel != "" {
			return nil, NotFoundError{Path: fs.joinLocal(rel)}
		}
		return nil, listErr
	}
	if rel != "" && len(items) == 0 {
		return nil, NotFoundError{Path: fs.joinLocal(rel)}
	}
	return items, nil
}

// ReadFile returns a handle that exposes the remote content as an io.ReadSeekCloser.
func (fs *FileSystem) ReadFile(ctx context.Context, local string) (*ReadHandle, error) {
	rel, err := fs.sanitize(local)
	if err != nil {
		return nil, err
	}
	if rel == "" {
		return nil, fmt.Errorf("cannot read directory %s", local)
	}
	absPath := fs.joinLocal(rel)
	path, err := fs.cache.LoadOrCreate(rel, func(f *os.File) (int64, error) {
		if err := fs.store.Download(ctx, rel, f); err != nil {
			return 0, err
		}
		info, err := f.Stat()
		if err != nil {
			return 0, err
		}
		return info.Size(), nil
	})
	if err != nil {
		if objectstore.IsNotFound(err) {
			return nil, NotFoundError{Path: absPath}
		}
		return nil, err
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open cache file: %w", err)
	}
	fs.cache.Touch(rel)
	return &ReadHandle{
		File: file,
	}, nil
}

// ReadHandle exposes cached readers.
type ReadHandle struct {
	*os.File
}

// WarmMetadataCache walks the entire remote tree and caches metadata locally so
// subsequent stats can be served without network hops.
func (fs *FileSystem) WarmMetadataCache(ctx context.Context) error {
	entries := make(map[string]objectstore.FileMeta)
	entries[""] = objectstore.FileMeta{Path: "", IsDir: true}
	if err := fs.populateMetadata(ctx, "", entries); err != nil {
		return err
	}
	fs.metaMu.Lock()
	fs.meta = entries
	fs.metaMu.Unlock()
	return nil
}

// cachedMeta returns the cached metadata entry when WarmMetadataCache has
// already enumerated the tree.
func (fs *FileSystem) cachedMeta(rel string) (objectstore.FileMeta, bool) {
	fs.metaMu.RLock()
	defer fs.metaMu.RUnlock()
	if fs.meta == nil {
		return objectstore.FileMeta{}, false
	}
	meta, ok := fs.meta[rel]
	return meta, ok
}

// populateMetadata recursively walks the remote namespace and stores every
// object/directory inside dst for later lookups.
func (fs *FileSystem) populateMetadata(ctx context.Context, rel string, dst map[string]objectstore.FileMeta) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	default:
	}
	items, err := fs.store.List(ctx, rel)
	if err != nil {
		if objectstore.IsNotFound(err) {
			return nil
		}
		return err
	}
	for _, item := range items {
		dst[item.Path] = item
		if item.IsDir {
			if err := fs.populateMetadata(ctx, item.Path, dst); err != nil {
				return err
			}
		}
	}
	return nil
}
