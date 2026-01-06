package remotefs

import (
	"context"
	"errors"
	"io"
	"path/filepath"
	"testing"

	"example.com/s3rofs/pkg/objectstore"
)

func TestSanitizeEnforcesLocalRoot(t *testing.T) {
	root := filepath.Join("var", "data", "remote")
	fs := &FileSystem{localRoot: filepath.Clean(root)}

	tests := []struct {
		name    string
		path    string
		want    string
		wantErr bool
	}{
		{
			name: "root",
			path: root,
			want: "",
		},
		{
			name: "child file",
			path: filepath.Join(root, "reports", "today.txt"),
			want: "reports/today.txt",
		},
		{
			name:    "sibling disallowed",
			path:    filepath.Join(root, "../other/file"),
			wantErr: true,
		},
		{
			name:    "prefix lookalike disallowed",
			path:    root + "-mirror/file",
			wantErr: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := fs.sanitize(tt.path)
			if tt.wantErr {
				if err == nil {
					t.Fatalf("expected error for %s", tt.name)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if got != tt.want {
				t.Fatalf("sanitize(%q) = %q, want %q", tt.path, got, tt.want)
			}
		})
	}
}

func TestSanitizeNoRoot(t *testing.T) {
	fs := &FileSystem{}
	path := filepath.Join(string(filepath.Separator), "alpha", "beta")
	got, err := fs.sanitize(path)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if got != "alpha/beta" {
		t.Fatalf("sanitize removed prefix incorrectly: got %q", got)
	}
}

func TestStatDetectsDirectoryViaListing(t *testing.T) {
	store := &statTestStore{
		listing: map[string][]objectstore.FileMeta{
			"docs": {
				{Path: "docs/report.txt"},
			},
		},
	}
	fs := &FileSystem{
		store: store,
	}
	localPath := filepath.Join(string(filepath.Separator), "docs")
	meta, err := fs.Stat(context.Background(), localPath)
	if err != nil {
		t.Fatalf("stat failed: %v", err)
	}
	if !meta.IsDir {
		t.Fatalf("expected IsDir=true, got %#v", meta)
	}
	if meta.Path != "docs" {
		t.Fatalf("stat returned path %q, want %q", meta.Path, "docs")
	}
}

type statTestStore struct {
	head      map[string]objectstore.FileMeta
	listing   map[string][]objectstore.FileMeta
	headErr   error
	headCalls int
	listCalls []string
}

func (s *statTestStore) Head(ctx context.Context, key string) (objectstore.FileMeta, error) {
	s.headCalls++
	if s.head != nil {
		if meta, ok := s.head[key]; ok {
			return meta, nil
		}
	}
	if s.headErr != nil {
		return objectstore.FileMeta{}, s.headErr
	}
	return objectstore.FileMeta{}, errors.New("not found")
}

func (s *statTestStore) List(ctx context.Context, key string) ([]objectstore.FileMeta, error) {
	s.listCalls = append(s.listCalls, key)
	if s.listing == nil {
		return nil, nil
	}
	if items, ok := s.listing[key]; ok {
		return items, nil
	}
	return nil, nil
}

func (s *statTestStore) Download(ctx context.Context, key string, dst io.WriterAt) error {
	return nil
}

func TestWarmMetadataCachePopulatesEntries(t *testing.T) {
	store := &statTestStore{
		listing: map[string][]objectstore.FileMeta{
			"": {
				{Path: "docs", IsDir: true},
				{Path: "readme.txt", Size: 10},
			},
			"docs": {
				{Path: "docs/report.txt", Size: 42},
				{Path: "docs/archive", IsDir: true},
			},
			"docs/archive": {
				{Path: "docs/archive/old.txt", Size: 5},
			},
		},
	}
	fs := &FileSystem{store: store}
	if err := fs.WarmMetadataCache(context.Background()); err != nil {
		t.Fatalf("warm cache: %v", err)
	}
	if _, ok := fs.cachedMeta("docs"); !ok {
		t.Fatalf("docs directory missing from cache")
	}
	meta, ok := fs.cachedMeta("docs/report.txt")
	if !ok {
		t.Fatalf("report missing from cache")
	}
	if meta.Size != 42 {
		t.Fatalf("report size mismatch: %d", meta.Size)
	}
	if _, ok := fs.cachedMeta("docs/archive/old.txt"); !ok {
		t.Fatalf("nested file missing from cache")
	}
}

func TestStatUsesCachedMetadata(t *testing.T) {
	store := &statTestStore{
		listing: map[string][]objectstore.FileMeta{
			"": {
				{Path: "docs", IsDir: true},
			},
			"docs": {
				{Path: "docs/report.txt", Size: 21},
			},
		},
		headErr: errors.New("head called"),
	}
	fs := &FileSystem{
		store: store,
	}
	if err := fs.WarmMetadataCache(context.Background()); err != nil {
		t.Fatalf("warm cache: %v", err)
	}
	local := filepath.Join(string(filepath.Separator), "docs", "report.txt")
	meta, err := fs.Stat(context.Background(), local)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if meta.Size != 21 {
		t.Fatalf("stat size mismatch: %d", meta.Size)
	}
	if store.headCalls != 0 {
		t.Fatalf("expected no head calls, got %d", store.headCalls)
	}
}
