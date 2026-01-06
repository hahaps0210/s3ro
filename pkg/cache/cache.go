package cache

import (
	"container/list"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"sync"
)

// Cache implements a simple disk backed LRU cache with a hard byte budget.
type Cache struct {
	dir      string
	maxBytes int64

	mu      sync.Mutex
	entries map[string]*cacheEntry
	order   *list.List
	used    int64
}

type cacheEntry struct {
	path string
	size int64
	elem *list.Element
}

// New creates the cache in the provided directory.
func New(dir string, maxBytes int64) (*Cache, error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fmt.Errorf("make cache dir: %w", err)
	}
	return &Cache{
		dir:      dir,
		maxBytes: maxBytes,
		entries:  make(map[string]*cacheEntry),
		order:    list.New(),
	}, nil
}

func (c *Cache) keyPath(key string) string {
	sum := sha256.Sum256([]byte(key))
	return filepath.Join(c.dir, hex.EncodeToString(sum[:]))
}

// LoadOrCreate ensures the key is present in the cache and returns the absolute
// path. When the key is missing, the fetch callback is invoked to populate it.
// The callback receives an *os.File implementing io.WriterAt and must return
// the final size of the object.
func (c *Cache) LoadOrCreate(key string, fetch func(f *os.File) (int64, error)) (string, error) {
	c.mu.Lock()
	if entry, ok := c.entries[key]; ok {
		c.order.MoveToFront(entry.elem)
		path := entry.path
		c.mu.Unlock()
		return path, nil
	}
	path := c.keyPath(key)
	c.mu.Unlock()

	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0o644)
	if err != nil {
		return "", fmt.Errorf("open cache file: %w", err)
	}
	defer file.Close()

	size, err := fetch(file)
	if err != nil {
		_ = os.Remove(path)
		return "", err
	}

	info, err := file.Stat()
	if err != nil {
		_ = os.Remove(path)
		return "", fmt.Errorf("stat cache file: %w", err)
	}
	size = info.Size()

	c.mu.Lock()
	defer c.mu.Unlock()
	if err := c.ensureCapacity(size); err != nil {
		_ = os.Remove(path)
		return "", err
	}
	elem := c.order.PushFront(key)
	c.entries[key] = &cacheEntry{
		path: path,
		size: size,
		elem: elem,
	}
	c.used += size
	return path, nil
}

func (c *Cache) ensureCapacity(need int64) error {
	if c.maxBytes <= 0 {
		return nil
	}
	for c.used+need > c.maxBytes && c.order.Len() > 0 {
		last := c.order.Back()
		key := last.Value.(string)
		entry := c.entries[key]
		_ = os.Remove(entry.path)
		c.used -= entry.size
		delete(c.entries, key)
		c.order.Remove(last)
	}
	if c.used+need > c.maxBytes {
		return fmt.Errorf("cache capacity %d bytes exceeded by %d", c.maxBytes, c.used+need)
	}
	return nil
}

// Touch marks the key as recently used to avoid premature eviction.
func (c *Cache) Touch(key string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if entry, ok := c.entries[key]; ok {
		c.order.MoveToFront(entry.elem)
	}
}

// Remove evicts a key from the cache immediately.
func (c *Cache) Remove(key string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	entry, ok := c.entries[key]
	if !ok {
		return
	}
	_ = os.Remove(entry.path)
	c.order.Remove(entry.elem)
	c.used -= entry.size
	delete(c.entries, key)
}
