Email str #where(s -> '@' in s)
Username str #min(2) #max(15)
User
  id u32 #key
  name Username
  email_address Email

db
  users Tab[User]
