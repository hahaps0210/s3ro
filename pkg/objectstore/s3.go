package objectstore

import (
	"context"
	"errors"
	"fmt"
	"io"
	"path"
	"strings"

	"github.com/aws/aws-sdk-go-v2/aws"
	"github.com/aws/aws-sdk-go-v2/service/s3"
	"github.com/aws/aws-sdk-go-v2/service/s3/types"
)

// S3Store implements the ObjectStore interface using an S3-compatible API.
type S3Store struct {
	client *s3.Client
	bucket string
	prefix string
}

// NewS3Store instantiates an ObjectStore backed by an AWS SDK client and the
// provided bucket/prefix pair.
func NewS3Store(client *s3.Client, bucket, prefix string) *S3Store {
	prefix = strings.Trim(prefix, "/")
	if prefix != "" && !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	return &S3Store{
		client: client,
		bucket: bucket,
		prefix: prefix,
	}
}

// key normalizes relative paths into fully qualified S3 object keys respecting
// the configured prefix.
func (s *S3Store) key(rel string) string {
	rel = path.Clean("/" + rel)
	rel = strings.TrimPrefix(rel, "/")
	if rel == "." {
		rel = ""
	}
	if rel == "" {
		return strings.TrimSuffix(s.prefix, "/")
	}
	if s.prefix == "" {
		return rel
	}
	return s.prefix + rel
}

// Head returns metadata for a single object by issuing an S3 HEAD request.
func (s *S3Store) Head(ctx context.Context, rel string) (FileMeta, error) {
	key := s.key(rel)
	head, err := s.client.HeadObject(ctx, &s3.HeadObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	})
	if err != nil {
		var notFound *types.NotFound
		if errors.As(err, &notFound) {
			return FileMeta{}, NotFoundError{Key: rel}
		}
		return FileMeta{}, fmt.Errorf("head %s: %w", rel, err)
	}
	return FileMeta{
		Path:         rel,
		Size:         aws.ToInt64(head.ContentLength),
		ETag:         aws.ToString(head.ETag),
		LastModified: aws.ToTime(head.LastModified),
	}, nil
}

// List enumerates the immediate children for the provided prefix using the S3
// ListObjectsV2 paginator.
func (s *S3Store) List(ctx context.Context, rel string) ([]FileMeta, error) {
	prefix := s.key(rel)
	if prefix != "" && !strings.HasSuffix(prefix, "/") {
		prefix += "/"
	}
	input := &s3.ListObjectsV2Input{
		Bucket:    aws.String(s.bucket),
		Delimiter: aws.String("/"),
	}
	if prefix != "" {
		input.Prefix = aws.String(prefix)
	}
	var out []FileMeta
	paginator := s3.NewListObjectsV2Paginator(s.client, input)
	for paginator.HasMorePages() {
		page, err := paginator.NextPage(ctx)
		if err != nil {
			return nil, fmt.Errorf("list %s: %w", rel, err)
		}
		for _, cp := range page.CommonPrefixes {
			name := strings.TrimSuffix(strings.TrimPrefix(aws.ToString(cp.Prefix), s.prefix), "/")
			if name == "" {
				continue
			}
			if rel != "" {
				name = path.Join(rel, name)
			}
			out = append(out, FileMeta{
				Path:  name,
				IsDir: true,
			})
		}
		for _, obj := range page.Contents {
			key := aws.ToString(obj.Key)
			if prefix != "" && key == prefix {
				continue
			}
			name := strings.TrimPrefix(key, s.prefix)
			name = strings.TrimPrefix(name, "/")
			if rel != "" {
				if !strings.HasPrefix(name, rel+"/") && name != rel {
					continue
				}
				if idx := strings.Index(strings.TrimPrefix(name, rel+"/"), "/"); idx != -1 {
					continue
				}
			} else if strings.Contains(name, "/") {
				continue
			}
			out = append(out, FileMeta{
				Path:         name,
				Size:         aws.ToInt64(obj.Size),
				ETag:         aws.ToString(obj.ETag),
				LastModified: aws.ToTime(obj.LastModified),
			})
		}
	}
	return out, nil
}

// Download streams the contents of an S3 object into dst and mirrors io.Copy
// semantics for the caller.
func (s *S3Store) Download(ctx context.Context, rel string, dst io.WriterAt) error {
	key := s.key(rel)
	obj, err := s.client.GetObject(ctx, &s3.GetObjectInput{
		Bucket: aws.String(s.bucket),
		Key:    aws.String(key),
	})
	if err != nil {
		var notFound *types.NoSuchKey
		if errors.As(err, &notFound) {
			return NotFoundError{Key: rel}
		}
		return fmt.Errorf("download %s: %w", rel, err)
	}
	defer obj.Body.Close()
	buf := make([]byte, 2*1024*1024)
	var offset int64
	for {
		n, readErr := obj.Body.Read(buf)
		if n > 0 {
			if _, err := dst.WriteAt(buf[:n], offset); err != nil {
				return fmt.Errorf("write %s: %w", rel, err)
			}
			offset += int64(n)
		}
		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				return nil
			}
			return fmt.Errorf("read %s: %w", rel, readErr)
		}
	}
}
