MoneyAmmount u64
Username str #min(1) #max(20)
User
  name Username #key
  birth_day Date #min(d"1900-01-01") #max(today())
  balance MoneyAmmount

SuspendedUser
  id u32 #key
  user User
  suspension_time DateTime

db
  all_users Tab[User]
  suspended_users Tab[SuspendedUser]

[mut]
task
  #description("deletes suspended users that are N days old")
  #execute_every(d"15days")

  for u in db.suspended_users
    if now() - u.suspension_time > d"30days"
      db.suspended_users.del(u.id).unwrap

[mut]
suspend_user(name Username) union(void, GetErr #err)
  u = db.all_users.get(name).try
  suspended_u = SuspendedUser(
    id: db.suspended_users.next_id(),
    user: u,
    suspension_time: now()
  )

  db.suspended_users.set(suspended_u)
  db.all_users.del(name).unwrap

names_of_adult_users() Set[Username]
  return db.all_users
    .where(u -> now() - u.birth_day >= d"18years")
    .distinct_set()

NotEnoughMoneyErr
  requested_ammount MoneyAmmount
  available_ammount MoneyAmmount

SameUserErr void

TranserMoneyResult union
  void
  SameUserErr #err
  NotEnoughMoneyErr #err
  GetErr #err

[mut]
transfer_money(source Username, dest Username, ammount MoneyAmmount) TranserMoneyResult
  if source == dest
    raise SameUserErr()

  source_u = db.all_users.get(source).try
  defer db.all_users.set(source_u)

  dest_u = db.all_users.get(dest).try
  defer db.all_users.set(dest_u)

  if source_u.balance < ammount
    raise NotEnoughMoneyErr(requested_ammount: ammount, available_ammount: MoneyAmmount(source_u.balance))

  source_u.balance -= ammount
  dest_u.balance += ammount
