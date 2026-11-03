evolve "v1.mg" v1
  for u in v1.db.users
    db.users.set(User(
      id: u64(u.id),
      username: u.name,
      phone: null,
      balance: 0,
      email: Email(u.email_address) else null
    ))

Email str #min(3) #max(20) #where(s -> '@' in s)
Username str #min(2) #max(15)
Phone str #min(8) #max(10)
Balance u64
User
  id u64 #key
  username str
  phone ?Phone
  balance Balance
  email ?Email

db
  users Tab[User]