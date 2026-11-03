evolve "v2.mg" v2
  for u in v2.db.users
    db.users.set(User(
      id: u32(u.id),
      name: u.username,
      email_address: u.email ?? Email("default@mail.com")
    ))

Email str #where(s -> '@' in s)
Username str #min(2) #max(15)
User
  id u32 #key
  name Username
  email_address Email

db
  users Tab[User]
