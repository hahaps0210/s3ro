package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"path"
	"strings"
	"testing"

	"example.com/s3rofs/pkg/objectstore"
	"example.com/s3rofs/pkg/remotefs"
)

func TestIPCServerHandlers(t *testing.T) {
	store := newFakeStore()
	cacheDir := t.TempDir()
	fs, err := remotefs.New(store, remotefs.Config{
		LocalRoot: "/data",
		CacheDir:  cacheDir,
		CacheSize: 1 << 20,
	})
	if err != nil {
		t.Fatalf("init remotefs: %v", err)
	}
	ipc, err := remotefs.NewIPCServer(fs)
	if err != nil {
		t.Fatalf("init IPC server: %v", err)
	}
	ts := httptest.NewServer(ipc.Handler())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/stat?path=/data/docs/report.txt")
	if err != nil {
		t.Fatalf("stat request: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("stat status = %d", resp.StatusCode)
	}
	var meta remotefs.POSIXEntry
	if err := json.NewDecoder(resp.Body).Decode(&meta); err != nil {
		t.Fatalf("decode stat: %v", err)
	}
	if meta.Path != "docs/report.txt" {
		t.Fatalf("stat returned wrong path %q", meta.Path)
	}

	resp, err = http.Get(ts.URL + "/ls?path=/data/docs")
	if err != nil {
		t.Fatalf("ls request: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("ls status = %d", resp.StatusCode)
	}
	var entries []remotefs.POSIXEntry
	if err := json.NewDecoder(resp.Body).Decode(&entries); err != nil {
		t.Fatalf("decode ls: %v", err)
	}
	if len(entries) != 1 || entries[0].Path != "docs/report.txt" {
		t.Fatalf("unexpected ls entries: %+v", entries)
	}

	resp, err = http.Get(ts.URL + "/cat?path=/data/docs/report.txt")
	if err != nil {
		t.Fatalf("cat request: %v", err)
	}
	body, err := io.ReadAll(resp.Body)
	resp.Body.Close()
	if err != nil {
		t.Fatalf("read cat body: %v", err)
	}
	if string(body) != "hello world" {
		t.Fatalf("cat returned %q", string(body))
	}
}

type fakeStore struct {
	files map[string]*fakeFile
}

type fakeFile struct {
	meta objectstore.FileMeta
	data []byte
}

func newFakeStore() *fakeStore {
	files := map[string]*fakeFile{
		"docs/report.txt": {
			meta: objectstore.FileMeta{
				Path: "docs/report.txt",
				Size: 11,
			},
			data: []byte("hello world"),
		},
	}
	return &fakeStore{files: files}
}

func (f *fakeStore) Head(ctx context.Context, key string) (objectstore.FileMeta, error) {
	if file, ok := f.files[key]; ok {
		return file.meta, nil
	}
	return objectstore.FileMeta{}, objectstore.NotFoundError{Key: key}
}

func (f *fakeStore) List(ctx context.Context, key string) ([]objectstore.FileMeta, error) {
	var result []objectstore.FileMeta
	dirSeen := make(map[string]struct{})
	prefix := key
	if prefix != "" && !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	for full, file := range f.files {
		if key == "" {
			if idx := strings.Index(full, "/"); idx != -1 {
				dir := full[:idx]
				if _, ok := dirSeen[dir]; !ok {
					dirSeen[dir] = struct{}{}
					result = append(result, objectstore.FileMeta{
						Path:  dir,
						IsDir: true,
					})
				}
				continue
			}
			result = append(result, file.meta)
			continue
		}
		if !strings.HasPrefix(full, prefix) {
			continue
		}
		rest := strings.TrimPrefix(full, prefix)
		if rest == "" {
			continue
		}
		if strings.Contains(rest, "/") {
			dir := strings.Split(rest, "/")[0]
			name := path.Join(key, dir)
			if _, ok := dirSeen[name]; !ok {
				dirSeen[name] = struct{}{}
				result = append(result, objectstore.FileMeta{
					Path:  name,
					IsDir: true,
				})
			}
			continue
		}
		result = append(result, objectstore.FileMeta{
			Path: path.Join(key, rest),
			Size: file.meta.Size,
		})
	}
	return result, nil
}

func (f *fakeStore) Download(ctx context.Context, key string, dst io.WriterAt) error {
	file, ok := f.files[key]
	if !ok {
		return objectstore.NotFoundError{Key: key}
	}
	if _, err := dst.WriteAt(file.data, 0); err != nil {
		return err
	}
	return nil
}
