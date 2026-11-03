| general concept: all manual BUT heavily assisted with debug builds full of helper errors/warning/reports

CommentID u64
Comment
  id CommentID #key
  user_id UserID
  content str #min(1) #max(50)
  likes Set[UserID]

PostID u64
Post
  id PostID #key
  image Blob
  user_id UserID
  likes Set[UserID]
  comments Set[CommentID]

UserID u32
User
  id UserID #key
  name str #min(3) #max(15) #unique
  picture Blob
  posts Set[PostID]

db
  posts Tab[Post]
  users Tab[User]

  comments Tab[Comment]

[mut]
submit_post(user_id UserID, image Blob) union(PostID, GetErr #err)
  user = db.users.get(user_id).try
  defer db.users.set(user)

  post_id = db.posts.next_id()
  db.posts.set(Post(id: post_id, image, user_id, likes: [] using db_alloc, comments: [] using db_alloc))

  user.posts.add(post_id).unwrap

  return post_id

[mut]
remove_post(post_id PostID) union(void, GetErr #err)
  post = db.posts.get(post_id).try
  defer db.posts.del(post_id)

  _ = db.comments.del_all(post.comments)
  
  user_id = post.user_id

  user = db.users.get(user_id).try
  defer db.users.set(user)

  user.posts.remove(post_id).unwrap

Action enum
  like
  unlike

[mut]
like_unlike_post(user_id UserID, post_id PostID, action Action) union(void, GetErr #err)
  if user_id not in db.users
    raise GetErr()
  
  post = db.posts.get(post_id).try
  defer db.posts.set(post)

  if action == Action.like
    _ = post.likes.add(user_id)
  else
    _ = post.likes.remove(user_id)

[mut]
like_unlike_comment(user_id UserID, comment_id CommentID, action Action) union(void, GetErr #err)
  if user_id not in db.users
    raise GetErr()
  
  comment = db.comments.get(comment_id).try
  defer db.comments.set(comment)

  if action == Action.like
    _ = comment.likes.add(user_id)
  else
    _ = comment.likes.remove(user_id)
